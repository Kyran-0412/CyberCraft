package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberCraftClient;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(Screen.class)
public abstract class ScreenMixin {
	/** Opening the inventory or a menu must not freeze Minecraft's world while Cyberpunk is the one showing it. */
	@Inject(method = "isPauseScreen", at = @At("HEAD"), cancellable = true)
	private void cybercraft$neverPause(CallbackInfoReturnable<Boolean> cir) {
		if (CyberCraftClient.overlayActive()) {
			cir.setReturnValue(false);
		}
	}
}
