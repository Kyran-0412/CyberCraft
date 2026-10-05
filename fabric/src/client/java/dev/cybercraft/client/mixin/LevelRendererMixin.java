package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.framegraph.FrameGraphBuilder;
import com.mojang.blaze3d.resource.GraphicsResourceAllocator;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import dev.cybercraft.client.CyberCraftClient;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Vector4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	private static final String RENDER = "render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V";

	/**
	 * While Cyberpunk is showing the world but Minecraft's blocks can't be lined up (no camera yet, or turned off with
	 * /ccworld), Minecraft doesn't draw its world at all: only the hand, HUD and screens are left on a transparent
	 * background. Same hook SkyCraft uses.
	 */
	@Inject(method = RENDER, at = @At("HEAD"), cancellable = true)
	private void cybercraft$skipLevel(GraphicsResourceAllocator allocator, boolean renderOutline, CameraRenderState camera, GpuBufferSlice fog, Vector4f fogColor,
		boolean renderSky, boolean other, CallbackInfo ci) {
		if (CyberCraftClient.overlayActive() && !CyberCraftClient.drawWorld()) {
			ci.cancel();
		} else if (CyberCraftClient.drawWorld()) {
			// Note what Minecraft is really about to draw through, to compare with what was asked for (see the log).
			CyberCraftClient.noteRenderCamera(camera.pos.x, camera.pos.y, camera.pos.z, camera.yRot, camera.xRot,
				camera.projectionMatrix.m00(), camera.projectionMatrix.m11());
		}
	}

	/** When the blocks are drawn: start from a transparent background, not the sky colour. */
	@ModifyVariable(method = RENDER, at = @At("HEAD"), argsOnly = true)
	private Vector4f cybercraft$transparentBackground(Vector4f clearColor) {
		return CyberCraftClient.drawWorld() ? new Vector4f(0.0f, 0.0f, 0.0f, 0.0f) : clearColor;
	}

	/** And without Minecraft's sky (sun, moon, stars): Cyberpunk has its own. */
	@Inject(method = "addSkyPass", at = @At("HEAD"), cancellable = true)
	private void cybercraft$noSky(FrameGraphBuilder builder, CameraRenderState camera, GpuBufferSlice fog, CallbackInfo ci) {
		if (CyberCraftClient.drawWorld()) {
			ci.cancel();
		}
	}
}
