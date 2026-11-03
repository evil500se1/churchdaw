import bpy, struct, os, re, json, argparse, sys
from mathutils import Vector
parser=argparse.ArgumentParser()
parser.add_argument('--blend',required=True)
parser.add_argument('--output',required=True)
args=parser.parse_args(sys.argv[sys.argv.index('--')+1:])
bpy.ops.wm.open_mainfile(filepath=os.path.abspath(args.blend))
scene=bpy.data.scenes['01 • Concert Grand']; bpy.context.window.scene=scene
hinge=scene.objects['Lid hinge • rotate Y to open']; hinge.rotation_euler.y=0
# Small bevel tessellation is enough for an interactive instrument view.
for obj in scene.objects:
    for m in obj.modifiers:
        if m.type=='BEVEL': m.segments=1
bpy.context.view_layer.update(); deps=bpy.context.evaluated_depsgraph_get()
data=bytearray(); keys=[]; objects=0
for obj in scene.objects:
    if obj.type not in ('MESH','CURVE','FONT') or obj.name=='Lid prop': continue
    match=re.search(r'MIDI (\d+)',obj.name)
    group=int(match.group(1))-20 if match else (89 if obj.parent==hinge else 0)
    if match: keys.append(int(match.group(1)))
    ev=obj.evaluated_get(deps); mesh=ev.to_mesh(); mesh.calc_loop_triangles()
    mat=ev.matrix_world; nm=mat.to_3x3().inverted().transposed()
    for tri in mesh.loop_triangles:
        material=mesh.materials[tri.material_index] if len(mesh.materials) else None
        col=list(material.diffuse_color[:3]) if material else [.2,.2,.2]
        for vi in tri.vertices:
            v=mesh.vertices[vi]; p=mat@v.co
            n=(nm@(v.normal if mesh.polygons[tri.polygon_index].use_smooth else tri.normal)).normalized()
            data.extend(struct.pack('<10f',*p,*n,*col,float(group)))
    ev.to_mesh_clear(); objects+=1
assert sorted(keys)==list(range(21,109))
path=os.path.abspath(args.output)
os.makedirs(os.path.dirname(path),exist_ok=True)
open(path,'wb').write(data)
print(json.dumps({'vertices':len(data)//40,'bytes':len(data),'keys':len(keys),'objects':objects}))
