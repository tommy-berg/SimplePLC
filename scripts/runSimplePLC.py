import os
import shutil

file_to_move = "simulation.lua"
target_subfolder = "build"
settings_file_path = os.path.join("..", "SimplePLC", "build", "settings.ini")
tags_file_path = "tags.ini"

def move_file():
    dest_folder = os.path.join("..", "SimplePLC" ,target_subfolder)
    os.makedirs(dest_folder, exist_ok=True)

    src_path = os.path.abspath(file_to_move)
    dest_path = os.path.join(dest_folder, os.path.basename("world.plc"))

    shutil.move(src_path, dest_path)
    print(f"Moved {file_to_move} to {dest_path}")

def replace_tags_section():
    if not os.path.exists(settings_file_path):
        print(f"Cannot find settings.ini at: {settings_file_path}")
        return
    if not os.path.exists(tags_file_path):
        print(f"Cannot find tags.ini in current directory.")
        return

    with open(settings_file_path, "r") as f:
        lines = f.readlines()

    with open(tags_file_path, "r") as f:
        new_tags = f.readlines()

    result = []
    inside_tags = False
    for line in lines:
        if line.strip().lower() == "[tags]":
            inside_tags = True
            result.extend(new_tags[1:]) 
            continue
        if inside_tags:
            if line.strip().startswith("[") and not line.strip().lower().startswith("[Tags]"):
                inside_tags = False
                result.append(line)
        elif not inside_tags:
            result.append(line)

    with open(settings_file_path, "w") as f:
        f.writelines(result)

    print(f"Replaced [Tags] section in {settings_file_path}")

if __name__ == "__main__":
    move_file()
    replace_tags_section()
