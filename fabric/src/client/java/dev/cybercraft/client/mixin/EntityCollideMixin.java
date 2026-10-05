package dev.cybercraft.client.mixin;

import dev.cybercraft.client.GroundCollider;
import dev.cybercraft.world.GroundCollision;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * After vanilla has collided the local player's movement with Minecraft blocks, collide it with Night
 * City's ground and obstacles (smooth slopes and wall sliding instead of block steps). Same hook SkyCraft uses.
 */
@Mixin(Entity.class)
public abstract class EntityCollideMixin {
	@Inject(method = "collide", at = @At("RETURN"), cancellable = true)
	private void cybercraft$smoothCollision(Vec3 movement, CallbackInfoReturnable<Vec3> cir) {
		if ((Object) this instanceof LocalPlayer player && GroundCollision.enabled() && GroundCollision.smoothPlayers() && !player.noPhysics) {
			cir.setReturnValue(GroundCollider.collide(player, cir.getReturnValue()));
		}
	}
}
