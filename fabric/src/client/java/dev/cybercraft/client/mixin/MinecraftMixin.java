package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberCraftClient;
import dev.cybercraft.client.FrameExporter;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	/** After Minecraft has drawn a frame (hand, HUD, screens): ship it to Cyberpunk. Same hook SkyCraft uses. */
	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void cybercraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		if (CyberCraftClient.overlayActive()) {
			FrameExporter.capture((Minecraft) (Object) this);
		}
	}
}
