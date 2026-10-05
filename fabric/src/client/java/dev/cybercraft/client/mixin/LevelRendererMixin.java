package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberCraftClient;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	/**
	 * While Cyberpunk is showing the world, Minecraft doesn't draw it: only the hand, HUD and screens are left,
	 * on a transparent background, ready to be drawn over Cyberpunk. Same hook SkyCraft uses.
	 */
	@Inject(
		method = "render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V",
		at = @At("HEAD"),
		cancellable = true
	)
	private void cybercraft$skipLevel(CallbackInfo ci) {
		if (CyberCraftClient.overlayActive()) {
			ci.cancel();
		}
	}
}
