package dev.cybercraft.client.mixin;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.backend.opengl.GlStateManager;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Reading back a depth texture sets the read framebuffer's read buffer to GL_NONE and never puts it back, so every later colour readback
 * through the same framebuffer fails ("No color buffer"). Vanilla never reads depth back; the layered export does every frame. Put the read
 * buffer back before the framebuffer is released. (Found and fixed the same way by the "Minecraft x GTA V" example in universal-modder,
 * MIT, by Rehan and contributors.)
 */
@Mixin(targets = "com.mojang.renderpearl.backend.opengl.GlCommandEncoder")
abstract class GlCommandEncoderMixin {
	private static final int GL_COLOR_ATTACHMENT0 = 0x8CE0;

	@Inject(
		method = "copyTextureToBuffer(Lcom/mojang/renderpearl/api/textures/GpuTexture;Lcom/mojang/renderpearl/api/buffers/GpuBuffer;JLjava/lang/Runnable;IIIII)V",
		at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/backend/opengl/GlStateManager;_glFramebufferTexture2D(IIIII)V"),
		require = 0
	)
	private void cybercraft$restoreReadBuffer(GpuTexture source, GpuBuffer destination, long offset, Runnable callback, int mipLevel, int x, int y, int width, int height, CallbackInfo ci) {
		if (source.getFormat().hasDepthAspect()) {
			GlStateManager._glReadBuffer(GL_COLOR_ATTACHMENT0);
		}
	}
}
