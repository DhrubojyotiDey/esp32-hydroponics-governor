import os

Import("env")

def generate_html_headers(source, target, env):
    src_dir = env.subst("$PROJECT_SRC_DIR")
    html_files = ["page_setup.html", "page_dash.html"]
    
    for f_name in html_files:
        src_path = os.path.join(src_dir, f_name)
        out_path = os.path.join(src_dir, f_name.replace(".", "_") + ".h")
        
        if not os.path.isfile(src_path):
            continue
            
        with open(src_path, "r", encoding="utf-8") as f:
            data = f.read().encode("utf-8")
        
        # Add a null terminator just in case users string-process it
        data += b'\x00'
            
        array_name = f_name.replace(".", "_")
        array_content = ", ".join(f"0x{b:02x}" for b in data)
        
        header_content = f"""// Auto-generated from {f_name}
#pragma once
#include <stdint.h>

const uint8_t {array_name}_start[] = {{ {array_content} }};
const uint8_t {array_name}_end[] = {{}}; // dummy for pointer math
const size_t {array_name}_len = {len(data) - 1};
"""
        # Only rewrite if changed (prevent rebuild loops)
        needs_write = True
        if os.path.isfile(out_path):
            with open(out_path, "r", encoding="utf-8") as f:
                needs_write = (f.read() != header_content)
                
        if needs_write:
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(header_content)
            print(f"Generated {out_path}")

# Run before anything compiles
env.Execute(generate_html_headers)
