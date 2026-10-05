package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.cybercraft.client.CyberCraftClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	/** Minecraft's own frame limit (and its lower limit when the window is unfocused) would hold the HUD back. */
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void cybercraft$fastEnough(CallbackInfoReturnable<Integer> cir) {
		if (CyberCraftClient.overlayActive()) {
			cir.setReturnValue(144);
		}
	}
}
