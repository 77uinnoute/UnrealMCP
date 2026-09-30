"""彻底清掉一套 SkeletalMesh 资产（physics -> skeleton -> mesh），供"重导换骨架"用。

背景（SKILL §二.5）：`import_assets(replace_existing=true)` **不会更新 skeleton**，
表现成"mesh 尺寸对了、骨骼还是旧比例"。要真换骨架必须**先删干净再导**。
三个资产互相引用，`safe_delete_asset` 会互相报 blockers，所以按顺序走 `delete_asset`，
再把残留的 `.uasset` 从磁盘删掉 + `scan_paths_synchronous`（否则同名 `rename_asset` 会失败）。

用法：改顶部参数 -> execute_python_file。
"""
import os

import unreal

# ---------------- 参数 ----------------
BASE = '/Game/MCP/Ganyu'          # 资产目录
STEM = 'Ganyu_UE'                 # 资产主干名：<STEM> / <STEM>_Skeleton / <STEM>_PhysicsAsset
EXTRA = []                        # 额外要删的资产（如旧 ABP）：['/Game/Blueprints/ABP_X']
DESTROY_ACTOR_LABELS = ['RT_Target_Ganyu']   # 先干掉引用它们的关卡 actor
DISK_DIR = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Content\MCP\Ganyu"
# --------------------------------------

EA = unreal.EditorAssetLibrary
AR = unreal.AssetRegistryHelpers.get_asset_registry()
w = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()

for a in unreal.GameplayStatics.get_all_actors_of_class(w, unreal.SkeletalMeshActor):
    if a.get_actor_label() in DESTROY_ACTOR_LABELS:
        print('DESTROY_ACTOR', a.get_actor_label(), a.destroy_actor())

for p in ('%s/%s_PhysicsAsset' % (BASE, STEM), '%s/%s_Skeleton' % (BASE, STEM), '%s/%s' % (BASE, STEM)) + tuple(EXTRA):
    print('DEL_ASSET', p, EA.delete_asset(p))
unreal.SystemLibrary.collect_garbage()

for f in (STEM + '.uasset', STEM + '_Skeleton.uasset', STEM + '_PhysicsAsset.uasset'):
    p = os.path.join(DISK_DIR, f)
    if os.path.exists(p):
        os.remove(p)
        print('RM_DISK', f)
AR.scan_paths_synchronous(BASE, True)

for p in ('%s/%s' % (BASE, STEM), '%s/%s_Skeleton' % (BASE, STEM)):
    print('EXISTS', p, EA.does_asset_exist(p), 'LOADABLE', EA.load_asset(p) is not None)
print('LEFT_IN_DIR', [str(a).split('/')[-1] for a in EA.list_assets(BASE, recursive=False)])
print('PURGE_DONE — 现在可以 import_assets 到同一目录（骨架会新建）')
