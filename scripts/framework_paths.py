Import("env")

import os

framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
env.Append(CPPPATH=[os.path.join(framework_dir, "libraries", "FS", "src")])
