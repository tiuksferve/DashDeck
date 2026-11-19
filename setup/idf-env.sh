# Ativação mínima do ESP-IDF v5.5.6 para ESP32-C6 (sem gdb/openocd, por falta de espaço)
unset VIRTUAL_ENV
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_TOOLS_PATH="$HOME/.espressif"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.11_env"
export ESP_ROM_ELF_DIR="$HOME/.espressif/tools/esp-rom-elfs/"
_tc=$(ls -d $HOME/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin | head -1)
export PATH="$IDF_PATH/tools:$IDF_PYTHON_ENV_PATH/bin:$_tc:$(echo "$PATH" | tr ':' '\n' | grep -vi venv | paste -sd:)"
unset _tc
