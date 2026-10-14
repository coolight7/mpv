
docker run -d \
  --name builder-mpv-2 \
  -v $(pwd):/home/mpv \
  -v /mnt/wslg:/mnt/wslg \
  -e PULSE_SERVER=/mnt/wslg/PulseServer \
  builer-mpv-env-2

export http_proxy=http://172.29.48.1:7897
export https_proxy=http://172.29.48.1:7897

meson setup build \
    --prefix=/home/mpv/build \
    --buildtype=debug \
    -Ddefault_library=static \
    -Dgpl=false \
    -Dlua=disabled \
    -Djavascript=disabled \
    -Dlibarchive=disabled \
    -Dvulkan=enabled \
    -Dtests=true

# 编译
meson compile -C build
 
# 安装
sudo meson install -C build

