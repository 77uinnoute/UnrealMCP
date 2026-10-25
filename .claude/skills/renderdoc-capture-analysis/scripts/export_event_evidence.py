"""Read-only evidence export inside RenderDoc MCP execute_python.

Call export_event(controller, rd, event_id, output_dir, previous_event,
                  capture_label). No imports of RenderDoc outside its host.
Does not export meshes, textures, push constants or API-specific blend/depth state.
"""
import json
import math
from pathlib import Path


def _variable(variable):
    """Keep reflected types and shape; never reinterpret integer storage as float."""
    kind = str(variable.type).rsplit('.', 1)[-1]
    item = {
        'name': variable.name, 'type': kind,
        'rows': variable.rows, 'columns': variable.columns,
        'members': [_variable(member) for member in variable.members],
    }
    if item['members']:
        return item
    fields = {
        'Float': 'f32v', 'Half': 'f32v', 'Double': 'f64v',
        'SInt': 's32v', 'UInt': 'u32v', 'Bool': 'u32v',
        'SLong': 's64v', 'ULong': 'u64v',
        'SShort': 's16v', 'UShort': 'u16v',
        'SByte': 's8v', 'UByte': 'u8v',
    }
    field = fields.get(kind)
    if field is None or not hasattr(variable.value, field):
        item['unsupported_type'] = True
    else:
        values = list(getattr(variable.value, field))[:variable.rows * variable.columns]
        item['values'] = [str(value) if isinstance(value, float) and not math.isfinite(value)
                          else value for value in values]
    return item


def export_event(controller, rd, event_id, output_dir, previous_event, capture_label):
    """Export VS/PS disassembly and buffer-backed typed constants, then restore event.

    previous_event must be the caller's current event (ctx.CurEvent()).
    output_dir must not exist. Inspect manifest errors/skips; partial export is
    evidence of missing data, never proof that shader has no constants.
    """
    if int(event_id) <= 0 or int(previous_event) < 0:
        raise ValueError('event_id must be positive; previous_event must be non-negative')
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=False)
    manifest = {
        'capture': str(capture_label), 'event_id': int(event_id),
        'previous_event': int(previous_event), 'stages': {}, 'errors': [],
        'scope': 'VS/PS disassembly and buffer-backed constants only; not a full pipeline dump',
    }
    try:
        controller.SetFrameEvent(int(event_id), True)
        pipeline = controller.GetPipelineState()
        manifest['graphics_pipeline'] = str(pipeline.GetGraphicsPipelineObject())
        for label, stage in [('vs', rd.ShaderStage.Vertex), ('ps', rd.ShaderStage.Pixel)]:
            data = {'shader': str(pipeline.GetShader(stage)), 'blocks': []}
            manifest['stages'][label] = data
            reflection = pipeline.GetShaderReflection(stage)
            if reflection is None:
                data['skipped'] = 'No shader reflection at this stage'
                continue
            data['entry_point'] = reflection.entryPoint
            try:
                text = controller.DisassembleShader(
                    pipeline.GetGraphicsPipelineObject(), reflection, '')
                if not text:
                    raise ValueError('Empty shader disassembly')
                filename = label + '_disassembly.txt'
                (out / filename).write_text(text, encoding='utf-8')
                data['disassembly'] = filename
            except Exception as exc:
                manifest['errors'].append({'stage': label, 'operation': 'disassemble', 'error': str(exc)})
            for index, block in enumerate(reflection.constantBlocks):
                entry = {'name': block.name, 'reflection_index': index,
                         'binding': block.fixedBindNumber, 'buffer_backed': block.bufferBacked}
                data['blocks'].append(entry)
                if not block.bufferBacked:
                    entry['skipped'] = 'Non-buffer-backed constants require API-specific extraction'
                    continue
                try:
                    descriptor = pipeline.GetConstantBlock(stage, index, 0).descriptor
                    entry.update(resource=str(descriptor.resource), byte_offset=descriptor.byteOffset,
                                 byte_size=descriptor.byteSize)
                    if descriptor.resource == rd.ResourceId.Null():
                        entry['skipped'] = 'No bound buffer'
                        continue
                    variables = controller.GetCBufferVariableContents(
                        pipeline.GetGraphicsPipelineObject(), pipeline.GetShader(stage), stage,
                        reflection.entryPoint, index, descriptor.resource,
                        descriptor.byteOffset, descriptor.byteSize)
                    entry['variables'] = [_variable(variable) for variable in variables]
                except Exception as exc:
                    entry['error'] = str(exc)
                    manifest['errors'].append({'stage': label, 'block': index, 'error': str(exc)})
    except Exception as exc:
        manifest['errors'].append({'operation': 'export_event', 'error': str(exc)})
        raise
    finally:
        try:
            controller.SetFrameEvent(int(previous_event), True)
            manifest['event_restored'] = True
        except Exception as exc:
            manifest['event_restored'] = False
            manifest['errors'].append({'operation': 'restore_event', 'error': str(exc)})
            raise
        finally:
            (out / 'manifest.json').write_text(
                json.dumps(manifest, indent=2, ensure_ascii=False, allow_nan=False), encoding='utf-8')
    return manifest
