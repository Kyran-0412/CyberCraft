package dev.cybercraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import java.lang.foreign.MemorySegment;
import net.minecraft.client.Minecraft;

/**
 * Copies Minecraft's main render target (hand + HUD + screens on a transparent background, because the world
 * isn't drawn while Cyberpunk is the one showing it) back from the GPU and publishes it to Cyberpunk through
 * the overlay triple buffer. Ported from SkyCraft's FrameExporter (MIT, by chasmlol).
 *
 * The copy is asynchronous: a frame is captured into one of a few staging buffers and shipped once the GPU
 * says the copy finished, typically a frame later.
 */
public final class FrameExporter {
	private static final int STAGING = 3;
	private static final int FREE = 0;
	private static final int PENDING = 1;
	private static final int READY = 2;
	// Capturing more often than this is a waste: Cyberpunk draws the newest one each of its frames anyway.
	private static final long MIN_CAPTURE_GAP_NANOS = 8_000_000L;

	private static final Staging[] staging = new Staging[STAGING];
	private static long nextFrameId = 1;
	private static long lastCapture;
	private static boolean loggedFormat;

	private static final class Staging {
		GpuBuffer buffer;
		int width;
		int height;
		volatile int state = FREE;
		long frameId;
	}

	private FrameExporter() {
	}

	private static boolean failedOnce;
	private static long framesCaptured;
	private static long framesShipped;
	private static long lastReport = System.nanoTime();

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
			CyberCraft.LOG.info("CyberCraft: HUD frames in the last 5 s: {} captured, {} sent to Cyberpunk", framesCaptured, framesShipped);
			framesCaptured = 0;
			framesShipped = 0;
		}
	}

	private static void captureInner(Minecraft minecraft) {
		shipReadyFrames();

		long now = System.nanoTime();
		if (now - lastCapture < MIN_CAPTURE_GAP_NANOS) {
			return;
		}

		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		if (!loggedFormat) {
			loggedFormat = true;
			CyberCraft.LOG.info("CyberCraft: overlay capture {}x{} format {}", width, height, color.getFormat());
		}

		Staging slot = null;
		for (Staging s : staging) {
			if (s != null && s.state == FREE) {
				slot = s;
				break;
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
		if (slot == null) {
			return; // all staging buffers still in flight; skip this frame
		}

		long bytes = (long) width * height * 4L;
		if (slot.buffer == null || slot.width != width || slot.height != height) {
			if (slot.buffer != null) {
				slot.buffer.close();
			}
			slot.buffer = RenderSystem.getDevice().createBuffer(() -> "CyberCraft overlay readback", 9, bytes);
			slot.width = width;
			slot.height = height;
		}
		final Staging captured = slot;
		captured.state = PENDING;
		captured.frameId = nextFrameId++;
		lastCapture = now;
		framesCaptured++;
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, captured.buffer, 0L, () -> captured.state = READY, 0);
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
			try (GpuBufferSlice.MappedView view = newest.buffer.map(true, false)) {
				MemorySegment src = MemorySegment.ofBuffer(view.data());
				MemorySegment.copy(src, 0, shm, CyberLink.overlayBackSlotOffset(), Math.min(bytes, src.byteSize()));
			}
			CyberLink.publishOverlay(newest.width, newest.height, true, newest.frameId);
			framesShipped++;
		}
		// Anything older than what we just shipped is useless now.
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId) {
				s.state = FREE;
			}
		}
	}
}
