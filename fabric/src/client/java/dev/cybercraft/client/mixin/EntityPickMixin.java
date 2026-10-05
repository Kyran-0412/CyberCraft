package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberCraftClient;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * What the player aims at (to break, place and use blocks) is worked out from the player's eyes and head angle. But the picture on
 * screen comes from Cyberpunk's camera, and the crosshair is the middle of that picture: if the two differ even slightly, the
 * crosshair points somewhere other than where you break. While Minecraft's blocks are drawn over Cyberpunk, aim along the camera's
 * own ray instead, from the camera through the middle of the screen. (The "Minecraft x GTA V" example in universal-modder, MIT,
 * does this for its third-person view; here it is for the first-person one.)
 */
@Mixin(Entity.class)
abstract class EntityPickMixin {
	@Inject(method = "pick(DFZ)Lnet/minecraft/world/phys/HitResult;", at = @At("HEAD"), cancellable = true)
	private void cybercraft$aimThroughTheCamera(double range, float partialTick, boolean withLiquids, CallbackInfoReturnable<HitResult> cir) {
		if (!CyberCraftClient.drawWorld() || !((Object) this instanceof LocalPlayer self)) {
			return;
		}
		Camera camera = Minecraft.getInstance().gameRenderer.mainCamera();
		Vec3 from = camera.position();
		Vec3 direction = new Vec3(camera.forwardVector()).normalize();
		Vec3 to = from.add(direction.scale(range));
		ClipContext.Fluid fluid = withLiquids ? ClipContext.Fluid.ANY : ClipContext.Fluid.NONE;
		cir.setReturnValue(self.level().clip(new ClipContext(from, to, ClipContext.Block.OUTLINE, fluid, self)));
	}
}
