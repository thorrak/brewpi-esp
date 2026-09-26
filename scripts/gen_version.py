# Generate the Version.h file
# Stitches together various bits of version information from the environment

import subprocess
import os

# This is permanently pinned to 0.2.4 for legacy reasons. The proper version incrementing happens in platformio.ini as FIRMWARE_REVISION
release = "0.2.4"

# Get the name of the "nearest" tag (decorated with revision information if
# changes have happened since that tag)
# See the man page of git-describe for more details
tag_name = (
    subprocess.check_output(["git", "describe", "--always"])
    .strip()
    .decode("utf-8")
)

# git revision
git_rev = (
    subprocess.check_output(["git", "rev-parse", "--short", "HEAD"])
    .strip()
    .decode("utf-8")
)



git_sha = subprocess.check_output(["git", "rev-parse", "HEAD"]).strip().decode("utf-8")
git_dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"]).strip())


template = f"""
#pragma once
/******************************************************************************
*** ****  WARNING **** ****

This file is auto-generated.  Any changes made here will be destroyed during
the next build.  To make persistent changes, edit the template in
/scripts/gen_version.py
******************************************************************************/

namespace Config {{
    namespace Version {{
        constexpr auto release = "{release}";
        constexpr auto git_tag = "{tag_name}";
        constexpr auto git_rev = "{git_rev}";
        constexpr auto git_sha = "{git_sha}";
        constexpr bool git_dirty = {str(git_dirty).lower()};
    }}
}};
"""

with open('src/Version.h', 'w') as f:
    f.write(template)
