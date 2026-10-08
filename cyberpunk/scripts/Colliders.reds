// CyberCraft: invisible physics boxes in Night City, so that cars are stopped by what you build in Minecraft.
//
// The plugin asks for each box by calling CyberCraftColliders.SpawnBox. This script spawns an empty entity and gives it its collider at the one
// moment the game allows it: while the entity is being set up (Codeware's "Entity/Initialize" event). A component added to an entity that is
// already finished is never initialised, and the game crashes when the entity is attached. (The plugin removes boxes again by itself.)
//
// Install: <game>\r6\scripts\CyberCraft\Colliders.reds (the build copies it). Needs Codeware, which needs redscript.
//
// The empty entity is base\spawner\empty_entity.ent, which comes with World Builder (Nexus Mods); it is the only thing CyberCraft takes from it.
// The collider is built the way the game structures it (and the way Codeware's documentation says to add components). The filter numbers are the
// ones that make a solid static box that blocks vehicles, the player and bullets. The preset name is ignored by the game here: the numbers decide.

public class CyberCraftColliderRequest extends IScriptable {
  public let halfExtents: Vector3;
}

public class CyberCraftColliderService extends ScriptableService {
  private let m_requests: array<ref<CyberCraftColliderRequest>>;

  private cb func OnInitialize() {
    GameInstance.GetCallbackSystem()
      .RegisterCallback(n"Entity/Initialize", this, n"OnEntityInitialize")
      .AddTarget(StaticEntityTarget.Tag(n"CyberCraftCollider"));
    FTLog("CyberCraft: collider service ready");
  }

  public func SpawnBox(position: Vector4, halfExtents: Vector3) -> EntityID {
    let request = new CyberCraftColliderRequest();
    request.halfExtents = halfExtents;
    ArrayPush(this.m_requests, request);

    let spec = new StaticEntitySpec();
    spec.templatePath = r"base\\spawner\\empty_entity.ent";
    spec.position = position;
    spec.tags = [n"CyberCraftCollider", StringToName("cybercraft_box_" + ToString(ArraySize(this.m_requests) - 1))];
    return GameInstance.GetStaticEntitySystem().SpawnEntity(spec);
  }

  private func FindRequestIndex(id: EntityID) -> Int32 {
    let tags = GameInstance.GetStaticEntitySystem().GetTags(id);
    for tag in tags {
      let name = NameToString(tag);
      if StrBeginsWith(name, "cybercraft_box_") {
        return StringToInt(StrAfterFirst(name, "cybercraft_box_"));
      }
    }
    return -1;
  }

  private cb func OnEntityInitialize(event: ref<EntityLifecycleEvent>) {
    let entity = event.GetEntity();
    if !IsDefined(entity) {
      return;
    }
    let index = this.FindRequestIndex(entity.GetEntityID());
    if index < 0 || index >= ArraySize(this.m_requests) {
      return;
    }
    this.AddBoxCollider(entity, this.m_requests[index]);
    this.m_requests[index] = null; // done with it (the list only ever grows otherwise)
  }

  private func AddBoxCollider(entity: ref<Entity>, request: ref<CyberCraftColliderRequest>) {
    let box = new physicsColliderBox();
    box.halfExtents = request.halfExtents;
    box.material = n"concrete.physmat";

    let query: QueryFilter;
    query.mask1 = Cast<Uint64>(0);
    query.mask2 = Cast<Uint64>(70107400);
    let simulation: SimulationFilter;
    simulation.mask1 = Cast<Uint64>(114696);
    simulation.mask2 = Cast<Uint64>(23627);
    let filter = new physicsFilterData();
    filter.preset = n"World Static";
    filter.queryFilter = query;
    filter.simulationFilter = simulation;

    let colliders: array<ref<physicsICollider>>;
    ArrayPush(colliders, box);

    let cls = Reflection.GetClass(n"entColliderComponent");
    let component = cls.MakeHandle();
    cls.GetProperty(n"name").SetValue(component, ToVariant(n"cybercraft_collider"));
    cls.GetProperty(n"colliders").SetValue(component, ToVariant(colliders));
    cls.GetProperty(n"filterData").SetValue(component, ToVariant(filter));
    let typed = component as IComponent;
    if IsDefined(typed) {
      entity.AddComponent(typed);
    } else {
      // The compiler doubts this cast (it only sees the script's own class tree); if it really fails, add the component through reflection instead.
      FTLog("CyberCraft: collider is not an IComponent here, adding it through reflection");
      Reflection.Call(entity, n"AddComponent", [ToVariant(component)]);
    }
  }
}

// Entry point for the plugin (a static function of a class, which the plugin finds among the game's global functions).
public class CyberCraftColliders extends IScriptable {
  public static func SpawnBox(x: Float, y: Float, z: Float, halfX: Float, halfY: Float, halfZ: Float) -> EntityID {
    let service = GameInstance.GetScriptableServiceContainer().GetService(n"CyberCraftColliderService") as CyberCraftColliderService;
    if !IsDefined(service) {
      FTLog("CyberCraft: collider service is not running");
      let none: EntityID;
      return none;
    }
    return service.SpawnBox(new Vector4(x, y, z, 1.0), new Vector3(halfX, halfY, halfZ));
  }
}
