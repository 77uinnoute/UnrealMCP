import unreal


def save_actors(actors=(), extra_packages=(), include_map=True, only_dirty=False):
    pkgs = set()
    for a in actors:
        try:
            p = a.get_package()
            if p:
                pkgs.add(p)
        except Exception:
            pass
    for p in extra_packages:
        pkgs.add(p if isinstance(p, unreal.Package) else unreal.load_package(p))
    if include_map:
        world = unreal.EditorLevelLibrary.get_editor_world()
        if world:
            pkgs.add(world.get_outer())
    if not pkgs:
        return False
    return unreal.EditorLoadingAndSavingUtils.save_packages(list(pkgs), only_dirty=only_dirty)


def save_assets(assets):
    ok = True
    for a in assets:
        ok = unreal.EditorAssetLibrary.save_asset(a.get_path_name()) and ok
    return ok


def save_modified_components(components, extra_packages=()):
    actors = set()
    for c in components:
        a = c.get_owner()
        if a:
            actors.add(a)
    return save_actors(list(actors), extra_packages)
