import os

basename_to_path = {}
for root, _, files in os.walk("."):
    if ".git" in root or "build" in root or "tools" in root or "userland" in root:
        continue
    for file in files:
        if file.endswith(".h") or file.endswith(".c"):
            path = os.path.normpath(os.path.join(root, file))
            # Store in a list in case of collisions
            if file not in basename_to_path:
                basename_to_path[file] = []
            basename_to_path[file].append(path)

def fix_file(file_path):
    with open(file_path, "r") as f:
        lines = f.readlines()
        
    changed = False
    for i, line in enumerate(lines):
        if line.strip().startswith("#include \""):
            start_idx = line.find('"') + 1
            end_idx = line.rfind('"')
            if start_idx > 0 and end_idx > start_idx:
                inc_path = line[start_idx:end_idx]
                
                target_abs = os.path.normpath(os.path.join(os.path.dirname(file_path), inc_path))
                if not os.path.exists(target_abs):
                    basename = os.path.basename(inc_path)
                    if basename in basename_to_path:
                        # If there are multiple, try to match the directory name if possible
                        options = basename_to_path[basename]
                        correct_abs = options[0]
                        if len(options) > 1:
                            # Heuristic: pick the one whose path matches the broken inc_path partially
                            for opt in options:
                                if os.path.basename(os.path.dirname(opt)) in inc_path:
                                    correct_abs = opt
                        
                        new_rel = os.path.relpath(correct_abs, os.path.dirname(file_path))
                        lines[i] = line[:start_idx] + new_rel + line[end_idx:]
                        changed = True
                    else:
                        print(f"Warning: {basename} not found for {file_path}")

    if changed:
        print("Fixed " + file_path)
        with open(file_path, "w") as f:
            f.writelines(lines)

for root, _, files in os.walk("."):
    if ".git" in root or "build" in root or "tools" in root or "userland" in root:
        continue
    for file in files:
        if file.endswith(".h") or file.endswith(".c") or file.endswith(".asm"):
            fix_file(os.path.normpath(os.path.join(root, file)))

# Also fix userland
for root, _, files in os.walk("userland"):
    if "build" in root: continue
    for file in files:
        if file.endswith(".h") or file.endswith(".c") or file.endswith(".asm"):
            fix_file(os.path.normpath(os.path.join(root, file)))
            
