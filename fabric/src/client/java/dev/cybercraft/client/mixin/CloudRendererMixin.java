package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberCraftClient;
import net.minecraft.client.renderer.CloudRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(CloudRenderer.class)
public abstract class CloudRendererMixin {
	/** Cyberpunk has its own sky: Minecraft's clouds must not be drawn over it. */
	@Inject(method = "render(Lnet/minecraft/client/CloudStatus;Lcom/mojang/renderpearl/api/commands/RenderPass;)V", at = @At("HEAD"), cancellable = true)
	private void cybercraft$noClouds(CallbackInfo ci) {
		if (CyberCraftClient.drawWorld()) {
			ci.cancel();
		}
	}

	@Inject(method = "renderOit", at = @At("HEAD"), cancellable = true)
	private void cybercraft$noCloudsOit(CallbackInfo ci) {
		if (CyberCraftClient.drawWorld()) {
			ci.cancel();
		}
	}
}
