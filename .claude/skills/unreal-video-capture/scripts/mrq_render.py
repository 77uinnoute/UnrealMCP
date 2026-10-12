"""Start a Movie Render Queue job that writes a PNG frame sequence.

Runs INSIDE the editor:  execute_python_file(file_path=<abs path to this file>)

Edit the constants below and re-run.  Nothing is persisted: the job lives in the
in-memory MoviePipelineQueue, so it must be rebuilt after every editor restart.
Requires the MovieRenderPipeline plugin to be enabled (see SKILL.md section 二).
"""
import os
import shutil

import unreal

SEQUENCE = "/Game/MCP/Demo/LS_Demo.LS_Demo"      # object path of the LevelSequence
LEVEL = "/Game/MCP/Demo/L_Demo.L_Demo"           # object path of the map to render in
OUT_DIR = r"E:\out\seq"                          # frames land here as f0000.png ...
RESOLUTION = (1280, 720)                         # MoviePipelineOutputSetting.output_resolution
FPS = (30, 1)                                    # use_custom_frame_rate / output_frame_rate
FRAME_STEP = 1                                   # >1 drops frames


def main():
    # never reuse an old directory: stale frames hide a render that failed to start
    if os.path.isdir(OUT_DIR):
        shutil.rmtree(OUT_DIR)

    sub = unreal.get_editor_subsystem(unreal.MoviePipelineQueueSubsystem)
    queue = sub.get_queue()
    queue.delete_all_jobs()

    job = queue.allocate_new_job(unreal.MoviePipelineExecutorJob)
    job.set_editor_property("job_name", "MCP")
    job.set_editor_property("sequence", unreal.SoftObjectPath(SEQUENCE))
    job.set_editor_property("map", unreal.SoftObjectPath(LEVEL))
    unreal.MoviePipelineEditorLibrary.ensure_job_has_default_settings(job)

    cfg = job.get_configuration()
    for setting in cfg.get_all_settings():                 # default output is JPG
        if setting.get_class().get_name().endswith("_JPG"):
            cfg.remove_setting(setting)
    cfg.find_or_add_setting_by_class(unreal.MoviePipelineImageSequenceOutput_PNG)

    out = cfg.find_or_add_setting_by_class(unreal.MoviePipelineOutputSetting)
    out.set_editor_property("output_directory", unreal.DirectoryPath(OUT_DIR))
    out.set_editor_property("file_name_format", "f{frame_number}")
    out.set_editor_property("zero_pad_frame_numbers", 4)
    out.set_editor_property("output_resolution", unreal.IntPoint(*RESOLUTION))
    out.set_editor_property("use_custom_frame_rate", True)
    out.set_editor_property("output_frame_rate", unreal.FrameRate(*FPS))
    out.set_editor_property("output_frame_step", FRAME_STEP)

    # class, not an instance
    sub.render_queue_with_executor(unreal.MoviePipelinePIEExecutor)
    print("MRQ started -> " + OUT_DIR)
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).get_current_level()
    print("editor level now: " + level.get_path_name())


main()
