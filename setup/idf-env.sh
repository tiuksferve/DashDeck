# Ativação mínima do ESP-IDF v5.5 para o ESP32-S3 (Xtensa). Usar com: . setup/idf-env.sh
# (o ~/esp/idf-env.sh do DashDeck ativa o toolchain RISC-V do C6; este ativa o Xtensa do S3)
unset VIRTUAL_ENV
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_TOOLS_PATH="$HOME/.espressif"
export IDF_PYTHON_ENV_PATH="$(ls -d $HOME/.espressif/python_env/idf5.5_py3.* | head -1)"
export ESP_ROM_ELF_DIR="$HOME/.espressif/tools/esp-rom-elfs/"
_tc=$(ls -d $HOME/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)
export PATH="$IDF_PATH/tools:$IDF_PYTHON_ENV_PATH/bin:$_tc:$(echo "$PATH" | tr ':' '\n' | grep -vi venv | paste -sd:)"
unset _tc
