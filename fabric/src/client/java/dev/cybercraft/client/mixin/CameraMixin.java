package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberCraftClient;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import org.joml.Quaternionf;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(Camera.class)
public abstract class CameraMixin {
	@Shadow
	protected abstract void setPosition(double x, double y, double z);

	@Shadow
	protected abstract void setRotation(float yRot, float xRot);

	@Shadow
	private Quaternionf rotation;

	/** While Minecraft's blocks are drawn over Cyberpunk, look through Cyberpunk's camera instead of the player's. */
	@Inject(method = "update", at = @At("RETURN"))
	private void cybercraft$useCyberpunkCamera(DeltaTracker deltaTracker, CallbackInfo ci) {
		if (CyberCraftClient.drawWorld()) {
			double[] pose = CyberCraftClient.cameraPose();
			this.setPosition(pose[0], pose[1], pose[2]);
			this.setRotation((float) pose[3], (float) pose[4]);
			if (CyberCraftClient.rollMode() != 0 && Math.abs(pose[5]) > 0.02) {
				// Minecraft's camera has no tilt of its own: roll the view about its own forward axis.
				this.rotation.rotateZ((float) Math.toRadians(pose[5]) * CyberCraftClient.rollMode());
			}
		}
	}

	/** And with Cyberpunk's field of view. */
	@Inject(method = "calculateFov", at = @At("RETURN"), cancellable = true)
	private void cybercraft$useCyberpunkFov(float partialTick, CallbackInfoReturnable<Float> cir) {
		if (CyberCraftClient.drawWorld()) {
			cir.setReturnValue((float) CyberCraftClient.cameraFov());
		}
	}
}
