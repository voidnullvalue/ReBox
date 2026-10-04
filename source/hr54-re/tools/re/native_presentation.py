#!/usr/bin/env python3
"""Reproduce native presentation evidence from extracted files, entirely offline.

Never imports a receiver transport, runs firmware, or opens a receiver device.
Call annotations use mips_elf's linear GOT tracker, not a CFG/data-flow proof.
The source instructions and manually verified findings remain authoritative.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from mips_elf import Elf
from rel_mips import RelElf

ROOT=Path(__file__).resolve().parents[2]
SPECS={
 'bist': ('opt/bist/bin/bist_main',r'^OPENGL_GFX_(Init|Term|ScreenUpdate)$',[(0x43dee0,0x43e248,'texture/quad/current helpers')]),
 'egl': ('opt/opengl/lib/libopengl.so.1',r'^egl(GetDisplay|Initialize|GetConfigs|CreateContext|CreateWindowSurface|MakeCurrent|DrawlistSetDepthDTV|SwapBuffers|QuerySurface|DestroySurface|DestroyContext|Terminate)$',[(0x598d0,0x59c0c,'deferred EGL object collection')]),
 'gl_draw': ('opt/opengl/lib/libopengl.so.1',r'^(glDrawArrays|get_draw_surface|shared_context_get_texture)$|^(InitDraw|DrawPrimitives|DrawPrimitive|getPrimitiveAssembler|gfx_blit_image|get_dlb_blend(?:_src|_dst)?|TriangleStripMergeAssembler::getNext|TriangleStripMergeAssembler::getVertices|TriangleStripMergeAssembler::getVertexCount|BasePrimitiveAssembler::isRectangle)\(',[]),
 'drawlist': ('opt/opengl/lib/libopengl.so.1',r'^(_dlSharedGetSurface|dlInit|dlInitSemaphores|dlGetAndAttachSharedMem|dlCreateHost|dlExitHost|dlExitHostFinal|dlCreateSurface|dlDestroySurface|_dlSurfaceUpdate|_dlSurfaceFree|_dlCompareSurfaces|dlSurfaceSetDepth|dlSurfaceGetResolution|dlSurfaceFlush|dlSurfaceSync|_dlHostWorkNotify|_dlMarkCells|dlSurfaceTexture|_dlFrameRender|_dlFrameExecuteCommandBuffer|dlRenderThreadsInit)$',[(0x14748,0x17120,'inferred render-thread entry and branches')]),
 'buffers': ('opt/opengl/lib/libopengl.so.1',r'^(dlTexture(Init|Allocate|GetFrameBuffer|GetID|GetUserOffset|GetDimensions|HasAlpha)|_dluTextureMap|dluTexture(Map|Sync|Unmap)|dluCalculatePitch|gl(TexImage2D|TexSubImage2D|GetMetaDataTexImage2D|Finish|Flush)|gfx_(create_image|allocate_image_data|copy_pixels)|dlBridge(CreateContext|SetDestination|Clear|Flush|Swap))$',[]),
 'ump_server': ('opt/dtvwm/lib/libdtvwm.so',r'(Session::(getVirtualSurfaceBitmap|getVirtualSurfaceOrderedList|getBitmapRef|refreshScreen|lockSurface|unlockSurface|setSurfaceZListPosition|damageRegion|removeBitmap|returnBitmap|configureShim)|VirtualBitmap::(VirtualBitmap|isVisible|setZListPosition|mapBuffer|initPbuffer)|VirtualDeviceCache::(getVirtualSurfaceBitmap|allocateBitmap|returnBitmap)|Shim::(configureSurface|swapBuffer)|IpcServer::(getVirtualSurfaceBitmap|damageRegion|setSurfaceZListPosition|returnBitmap))\(',[]),
 'ump_client': ('opt/dtvwm/lib/libdtvwmipcclient.so',r'IpcClient::(getVirtualSurfaceBitmap|damageRegion|setSurfaceZListPosition|returnBitmap|VirtualSurfaceInfo::(getSharedContigMemDeviceName|syncMem))\(',[]),
 'widgets': ('opt/native_widgets/lib/libnativewidgets.so',r'(CWidgetManager::(nativeWidgetInit|clientInit)|CTextRenderer::(initialize|renderText|swapToDisplayAndDrawPbuffer)|DtvCIFrame::initIFrameSurface)\(',[]),
 'clutter': ('opt/clutter-devel/lib/libclutter.so',r'^clutter_stage_egl_(realize|unrealize|redraw|paint)$',[]),
 'dtvwm_main': ('opt/dtvwm/bin/dtvwm',r'^main$',[]),
 'font': ('opt/libufont/lib/libufont.so',r'^FT_(Init_FreeType|New_Face|Load_Char|Render_Glyph|Set_Pixel_Sizes)$',[]),
}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rootfs',type=Path,default=ROOT/'extracted/sdb4-rootfs')
    p.add_argument('--output',type=Path,default=ROOT/'docs/native-ui-evidence')
    a=p.parse_args(); a.output.mkdir(parents=True,exist_ok=True)
    cache={}; manifest={}; edges=[]
    for name,(path,pattern,ranges) in SPECS.items():
        binary=a.rootfs/path
        if path not in cache:
            cache[path]=Elf(binary)
            manifest[path]={'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                            'address_kind':'ELF virtual address, before relocation/load bias'}
        e=cache[path]; chosen={(s['address'],s['size']):s['demangled'] for s in e.funcs
                              if re.search(pattern,s['demangled'])}
        for lo,hi,label in ranges: chosen[lo,hi-lo]=label
        with (a.output/(name+'.asm')).open('w') as out:
            out.write(f'# {path}\n# GOT call notes are linear annotations; verify delay slots and register writes.\n')
            for (start,size),label in sorted(chosen.items()):
                out.write(f'\n{label} @ 0x{start:x} size 0x{size:x}\n')
                for ins,note in e.annotate(start,start+size):
                    out.write(f'{ins.address:08x} {ins.mnemonic:8s} {ins.op_str:40s} # {note}\n')
                    if ins.mnemonic in ('jal','jalr') and note:
                        edges.append({'binary':path,'caller':label,'at':hex(ins.address),
                                      'annotation':note,'status':'linear annotation, not CFG proof'})
        print(f'{name}: {len(chosen)} function/range records')
    tables=[]
    for path,pattern in [
        ('opt/opengl/lib/libopengl.so.1',r'^vtable for TriangleStripMergeAssembler$'),
        ('opt/dtvwm/lib/libdtvwm.so',r'^vtable for directv::dtvwm::(Session|VirtualBitmap|VirtualSurface)$')]:
        e=cache[path]
        for s in e.symbols:
            if s['section']=='UND' or not re.search(pattern,s['demangled']): continue
            entries=[]
            for offset in range(0,s['size']-3,4):
                value=struct.unpack('>I',e.read(s['address']+offset,4))[0]
                reloc=e.relocations.get(s['address']+offset)
                if reloc: value=(value+reloc[0])&0xffffffff
                entries.append({'dump_offset':hex(offset),'value':hex(value),
                                'symbol':e.byaddr.get(value,{}).get('demangled',reloc[1] if reloc else '')})
            tables.append({'binary':path,'name':s['demangled'],'address':hex(s['address']),
                           'note':'Dump offsets include RTTI headers; not runtime vtable slots.',
                           'entries':entries})
    e=cache['opt/opengl/lib/libopengl.so.1']
    constructors=[hex((0xa7c00+offset)&0xffffffff)
                  for offset in struct.unpack('>7i',e.read(0x8d660,28))]
    tables.append({'binary':'opt/opengl/lib/libopengl.so.1','name':'getPrimitiveAssembler mode 0..6',
                   'address':'0x8d660','gp':'0xa7c00','constructor_targets':constructors})
    (a.output/'presentation_tables.json').write_text(json.dumps(tables,indent=2)+'\n')
    path='lib/modules/cdi/europa.ko'; binary=a.rootfs/path; e=RelElf(binary)
    manifest[path]={'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                    'address_kind':'ET_REL section-relative offset; NEVER a loaded address'}
    selected={
        (s['section'],s['address'],s['size']):s['name'] for s in e.symbols
        if s['kind']==2 and s['size'] and re.search(
            r'^(fb_(init|swap_buffers|handle_vsync|set_swap_interval|set_resolution)|'
            r'get_callisto_bcom_handles|get_cdi_driver_entry_points|mem_register_frame_buffer_offsets|bcdi_gko_register_fb_attach_callback|'
            r'bcdi_display_graphics_(blender_open|set_zlist)|bcdi_gko_shim_surface_create|bcdi_gko_fb_attach_callback)$',s['name'])}
    sec=e.names.index('.text')
    for lo,hi,label in [(0xb914c,0xb9244,'inferred grctl ioctl dispatcher'),
                        (0x65628,0x65674,'cdi +8 framebuffer attach indirection'),
                        (0x5fdd0,0x5ff54,'inferred NEXUS framebuffer attach callback')]:
        selected[sec,lo,hi-lo]=label
    with (a.output/'kernel.asm').open('w') as out:
        out.write('# europa.ko: relocations annotated, not applied. All addresses are section offsets.\n')
        for (sec,start,size),label in sorted(selected.items()):
            out.write(f'\n{label} @ {e.names[sec]}+0x{start:x} size 0x{size:x}\n')
            for ins,note in e.disasm(sec,start,size):
                out.write(f'{ins.address:08x} {ins.mnemonic:8s} {ins.op_str:40s} # {note}\n')
        ro=e.section_data(e.names.index('.rodata'))
        out.write('\ngrctl ioctl nr 0..6 jump table @ .rodata+0x4c13c0\n')
        for nr,target in enumerate(struct.unpack_from('>7I',ro,0x4c13c0)):
            out.write(f'  {nr}: .text+0x{target:x}\n')
    scripts={}
    for path in ['etc/init.d/S90ucentric','opt/middleware_core/bin/run_ucentric.sh',
                 'opt/middleware_core/bin/runsiege.sh']:
        data=(a.rootfs/path).read_bytes()
        scripts[path]={'sha256':hashlib.sha256(data).hexdigest()}
    manifest.update(scripts)
    (a.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (a.output/'call_annotations.json').write_text(json.dumps(edges,indent=2)+'\n')
    print('kernel: section-relative code, grctl table; manifest and call annotations written')

if __name__=='__main__': main()
