package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.cybercraft.client.CyberCraftClient;
import dev.cybercraft.client.InputBridge;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	/** While the keyboard comes from Cyberpunk, "is this key down?" asks the virtual keyboard, not the real one. */
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (CyberCraftClient.inputRouted()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	/** Minecraft's window is not the one being played in: it must not capture the real mouse. */
	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (CyberCraftClient.inputRouted()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (CyberCraftClient.inputRouted()) {
			ci.cancel();
		}
	}
}
