"""dump 骨架骨名 + 世界坐标 -> JSON/TSV（只读）。

为什么需要它：MCP 桥读脚本文件是按 4 KB 分块解码的，**脚本源码里带非 ASCII（日文/中文骨名）
会在块边界炸 `'utf-8' codec can't decode bytes`**。所以：
  本脚本（纯 ASCII）负责把骨名落盘；其它脚本从 JSON 按**下标**取骨名 -> 全程 ASCII 源码。

产物：
  <OUT_JSON>  [{"i":0,"name":"\\u64cd\\u4f5c\\u4e2d\\u5fc3","p":[x,y,z]}, ...]   (ensure_ascii)
  <OUT_TSV>   i<TAB>name<TAB>x<TAB>y<TAB>z                                     (真字符，供人读)
"""
import json
import os

import unreal

# ---------------- 参数 ----------------
MESH = '/Game/MCP/Ganyu/Ganyu_UE'
OUT_JSON = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\ue_bones.json"
OUT_TSV = r"e:\ue_proj\asset_test_UE55 (2)\asset_test_UE55\Saved\MCPAssets\MMD_Ganyu\ue_bones.tsv"
# --------------------------------------

mesh = unreal.EditorAssetLibrary.load_asset(MESH)
sk = mesh.get_editor_property('skeleton')
rp = unreal.AnimPoseExtensions.get_reference_pose(sk)
W = unreal.AnimPoseSpaces.WORLD
names = [str(n) for n in unreal.AnimPoseExtensions.get_bone_names(rp)]
rows = []
for i, n in enumerate(names):
    t = unreal.AnimPoseExtensions.get_bone_pose(rp, n, W).translation
    rows.append({'i': i, 'name': n, 'p': [round(t.x, 3), round(t.y, 3), round(t.z, 3)]})
with open(OUT_JSON, 'w', encoding='utf-8') as f:
    json.dump(rows, f, ensure_ascii=True, indent=1)
with open(OUT_TSV, 'w', encoding='utf-8') as f:
    for r in rows:
        f.write("%d\t%s\t%s\n" % (r['i'], r['name'], "\t".join(str(v) for v in r['p'])))
print('DUMPED', len(rows), OUT_JSON)
os.path.exists(OUT_TSV) and print('TSV', OUT_TSV)
print('DUMP_DONE')
