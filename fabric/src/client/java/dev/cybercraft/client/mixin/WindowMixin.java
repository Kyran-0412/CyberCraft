package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.cybercraft.client.CyberCraftClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(Window.class)
public abstract class WindowMixin {
	/**
	 * While Cyberpunk is the window being played in, Minecraft's own window never has focus. Say it does, so
	 * Minecraft doesn't pause itself ("pause on lost focus") or slow down. Same hook SkyCraft uses.
	 */
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void cybercraft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (CyberCraftClient.overlayActive()) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void cybercraft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (CyberCraftClient.overlayActive()) {
			cir.setReturnValue(false);
		}
	}
}
