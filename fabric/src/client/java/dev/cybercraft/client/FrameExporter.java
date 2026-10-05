package dev.cybercraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import java.lang.foreign.MemorySegment;
import java.util.concurrent.atomic.AtomicInteger;
import net.minecraft.client.Minecraft;
import org.joml.Vector4f;

/**
 * Copies Minecraft's picture back from the GPU and publishes it to Cyberpunk through the overlay triple buffer. Ported from
 * SkyCraft's FrameExporter (MIT, by chasmlol); the layered mode follows the approach of the "Minecraft x GTA V" example in
 * universal-modder (MIT, by Rehan and contributors): the frame is split into a world layer and an overlay layer.
 *
 * Two modes:
 *  - Plain: while the world isn't being drawn (or occlusion is off), the whole frame (hand, HUD, screens on a transparent
 *    background) is copied once, at the end of the frame.
 *  - Layered (blocks drawn over Cyberpunk, with depth): just before the hand is drawn, the world's colour and depth are copied and
 *    the colour target is cleared, so that what follows (hand, hotbar, screens) forms the overlay layer, copied at the end of the
 *    frame. Cyberpunk then draws the world layer only where it is nearer than the game's own depth, and the overlay over everything.
 *
 * The copies are asynchronous: a frame is captured into one of a few staging entries and shipped once the GPU says every copy
 * finished, typically a frame later.
 */
public final class FrameExporter {
	private static final int STAGING = 4;
	private static final int FREE = 0;
	private static final int PENDING = 1;
	private static final int READY = 2;
	// Capturing more often than this is a waste: Cyberpunk draws the newest one each of its frames anyway.
	private static final long MIN_CAPTURE_GAP_NANOS = 2_000_000L;
	private static final long STUCK_NANOS = 1_000_000_000L;
	private static final float NEAR = 0.05f;
	private static final Vector4f TRANSPARENT = new Vector4f(0.0f, 0.0f, 0.0f, 0.0f);

	private static final Staging[] staging = new Staging[STAGING];
	private static Staging pendingWorld; // a layered capture waiting for its overlay layer
	private static long nextFrameId = 1;
	private static long lastCapture;
	private static boolean loggedFormat;
	private static volatile boolean depthUnsupported;

	private static final class Staging {
		GpuBuffer color; // plain: the whole frame; layered: the world's colour
		GpuBuffer depth; // layered only
		GpuBuffer overlay; // layered only
		int width;
		int height;
		volatile int state = FREE;
		final AtomicInteger remaining = new AtomicInteger();
		long since;
		long frameId;
		long cameraFrame;
		boolean layered;
		float mcA;
		float mcB;
		boolean zeroToOne;

		void copyDone() {
			if (remaining.decrementAndGet() <= 0) {
				state = READY;
			}
		}
	}

	private FrameExporter() {
	}

	private static boolean failedOnce;
	private static long framesCaptured;
	private static long framesShipped;
	private static long layeredShipped;
	private static long lastReport = System.nanoTime();

	/** Has the world's depth turned out to be in a format that can't be exported? Layered mode is then off for good. */
	public static boolean depthUnsupported() {
		return depthUnsupported;
	}

	/** GameRenderer.renderLevel, just before the hand and 3D HUD are drawn: copy the world layer, then clear colour for the overlay. */
	public static void captureWorld(Minecraft minecraft) {
		pendingWorld = null; // one left from an interrupted frame is no use
		if (!CyberCraftClient.occludeActive()) {
			return;
		}
		try {
			captureWorldInner(minecraft);
		} catch (Throwable t) {
			if (!failedOnce) {
				failedOnce = true;
				CyberCraft.LOG.error("CyberCraft: copying the world layer out of Minecraft failed (this is reported once)", t);
			}
			pendingWorld = null;
		}
	}

	/** End of the frame: the overlay layer is complete (layered mode), or the whole frame is (plain mode). */
	public static void capture(Minecraft minecraft) {
		try {
			captureInner(minecraft);
		} catch (Throwable t) {
			if (!failedOnce) {
				failedOnce = true;
				CyberCraft.LOG.error("CyberCraft: copying the HUD out of Minecraft failed (this is reported once)", t);
			}
		}
		long now = System.nanoTime();
		if (now - lastReport > 5_000_000_000L) {
			lastReport = now;
			CyberCraft.LOG.info("CyberCraft: frames in the last 5 s: {} captured, {} sent to Cyberpunk ({} of them layered, with depth); hiding behind the game's world: {} (on: {}, drawing the world: {}, depth format unsupported: {})",
				framesCaptured, framesShipped, layeredShipped, CyberCraftClient.occludeActive() ? "active" : "NOT active", CyberCraftClient.occludeEnabled(), CyberCraftClient.drawWorld(), depthUnsupported);
			framesCaptured = 0;
			framesShipped = 0;
			layeredShipped = 0;
		}
	}

	private static Staging acquire(long now) {
		Staging slot = null;
		for (Staging s : staging) {
			if (s != null && s.state == PENDING && now - s.since > STUCK_NANOS) {
				s.state = FREE; // a copy that never finished: recycle it
			}
			if (s != null && s.state == FREE && slot == null) {
				slot = s;
			}
		}
		if (slot == null) {
			for (int i = 0; i < STAGING; i++) {
				if (staging[i] == null) {
					staging[i] = slot = new Staging();
					break;
				}
			}
		}
		return slot;
	}

	private static GpuBuffer ensureBuffer(GpuBuffer existing, boolean keep, long bytes, String name) {
		if (keep && existing != null) {
			return existing;
		}
		if (existing != null) {
			existing.close();
		}
		return RenderSystem.getDevice().createBuffer(() -> name, 9, bytes);
	}

	private static void captureWorldInner(Minecraft minecraft) {
		shipReadyFrames();

		long now = System.nanoTime();
		if (now - lastCapture < MIN_CAPTURE_GAP_NANOS) {
			return;
		}
		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		GpuTexture depth = target.getDepthTexture();
		if (color == null || depth == null) {
			return;
		}
		if (!"D32_FLOAT".equals(depth.getFormat().name())) {
			if (!depthUnsupported) {
				depthUnsupported = true;
				CyberCraft.LOG.warn("CyberCraft: Minecraft's depth texture is {}, not D32_FLOAT: hiding blocks behind the game's world is off", depth.getFormat());
			}
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		Staging slot = acquire(now);
		if (slot == null) {
			return; // all staging entries still in flight; skip this frame
		}

		long bytes = (long) width * height * 4L;
		boolean same = slot.width == width && slot.height == height && slot.layered;
		slot.color = ensureBuffer(slot.color, same, bytes, "CyberCraft world colour readback");
		slot.depth = ensureBuffer(slot.depth, same, bytes, "CyberCraft world depth readback");
		slot.overlay = ensureBuffer(slot.overlay, same, bytes, "CyberCraft overlay readback");
		slot.width = width;
		slot.height = height;
		slot.layered = true;

		// Minecraft's projection: window depth = ndc depth (or half of ndc + 1), and ndc depth = -m22 + m32 / distance.
		slot.mcA = CyberCraftClient.projectionM22();
		slot.mcB = CyberCraftClient.projectionM32();
		float ndcAtNear = -slot.mcA + slot.mcB / NEAR;
		slot.zeroToOne = ndcAtNear > -0.5f;

		slot.state = PENDING;
		slot.since = now;
		slot.remaining.set(3);
		slot.frameId = nextFrameId++;
		slot.cameraFrame = CyberCraftClient.cameraFrame();
		lastCapture = now;
		framesCaptured++;

		final Staging captured = slot;
		CommandEncoder encoder = RenderSystem.getDevice().createCommandEncoder();
		encoder.copyTextureToBuffer(color, captured.color, 0L, captured::copyDone, 0);
		encoder.copyTextureToBuffer(depth, captured.depth, 0L, captured::copyDone, 0);
		encoder.clearColorTexture(color, TRANSPARENT);
		pendingWorld = captured;
	}

	private static void captureInner(Minecraft minecraft) {
		shipReadyFrames();

		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}

		Staging world = pendingWorld;
		pendingWorld = null;
		if (world != null) {
			// Layered: what has been drawn since the world was copied (hand, hotbar, screens) is the overlay.
			if (target.width != world.width || target.height != world.height) {
				world.state = FREE;
				return;
			}
			RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, world.overlay, 0L, world::copyDone, 0);
			return;
		}
		if (CyberCraftClient.occludeActive()) {
			return; // the world layer was skipped this frame: the target holds a mix, send nothing
		}

		long now = System.nanoTime();
		if (now - lastCapture < MIN_CAPTURE_GAP_NANOS) {
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		if (!loggedFormat) {
			loggedFormat = true;
			CyberCraft.LOG.info("CyberCraft: overlay capture {}x{} format {}, depth format {}", width, height, color.getFormat(),
				target.getDepthTexture() != null ? target.getDepthTexture().getFormat() : "none");
		}

		Staging slot = acquire(now);
		if (slot == null) {
			return;
		}
		long bytes = (long) width * height * 4L;
		boolean same = slot.width == width && slot.height == height && !slot.layered;
		slot.color = ensureBuffer(slot.color, same, bytes, "CyberCraft overlay readback");
		slot.width = width;
		slot.height = height;
		slot.layered = false;
		final Staging captured = slot;
		captured.state = PENDING;
		captured.since = now;
		captured.remaining.set(1);
		captured.frameId = nextFrameId++;
		captured.cameraFrame = CyberCraftClient.cameraFrame();
		lastCapture = now;
		framesCaptured++;
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, captured.color, 0L, captured::copyDone, 0);
	}

	/** Maps the newest finished readback and copies it into shared memory. */
	private static void shipReadyFrames() {
		Staging newest = null;
		for (Staging s : staging) {
			if (s != null && s.state == READY && (newest == null || s.frameId > newest.frameId)) {
				newest = s;
			}
		}
		if (newest == null) {
			return;
		}
		MemorySegment shm = CyberLink.segment();
		if (shm != null) {
			long bytes = (long) newest.width * newest.height * 4L;
			long base = CyberLink.overlayBackSlotOffset();
			copy(newest.color, shm, base, bytes);
			int flags = Proto.SHF_BOTTOM_UP;
			if (newest.layered) {
				copy(newest.depth, shm, base + Proto.OVERLAY_LAYER_BYTES, bytes);
				copy(newest.overlay, shm, base + 2 * Proto.OVERLAY_LAYER_BYTES, bytes);
				flags |= Proto.SHF_LAYERED | (newest.zeroToOne ? Proto.SHF_ZERO_TO_ONE : 0);
				layeredShipped++;
			}
			CyberLink.publishOverlay(newest.width, newest.height, flags, newest.frameId, newest.cameraFrame, newest.mcA, newest.mcB, NEAR, 0.0f);
			framesShipped++;
		}
		// Anything older than what we just shipped is useless now.
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId) {
				s.state = FREE;
			}
		}
	}

	private static void copy(GpuBuffer buffer, MemorySegment shm, long offset, long bytes) {
		try (GpuBufferSlice.MappedView view = buffer.map(true, false)) {
			MemorySegment src = MemorySegment.ofBuffer(view.data());
			MemorySegment.copy(src, 0, shm, offset, Math.min(bytes, src.byteSize()));
		}
	}
}
