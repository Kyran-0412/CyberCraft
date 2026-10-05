package dev.cybercraft.client.mixin;

import dev.cybercraft.client.FrameExporter;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class GameRendererMixin {
	/**
	 * Just before the hand and 3D HUD are drawn, the world is complete: copy it (colour and depth) for Cyberpunk, then clear the colour
	 * so that the hand, hotbar and screens drawn next form a separate overlay layer. (The same split point as the "Minecraft x GTA V"
	 * example in universal-modder, MIT, by Rehan and contributors.)
	 */
	@Inject(
		method = "renderLevel",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/client/renderer/GameRenderer;render3dHud(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/OptionsRenderState;Z)V"
		)
	)
	private void cybercraft$captureWorld(CallbackInfo ci) {
		FrameExporter.captureWorld(Minecraft.getInstance());
	}
}
