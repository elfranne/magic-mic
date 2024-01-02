# Must be run with DOCKER_BUILDKIT=1
# syntax=docker/dockerfile:experimental
ARG TARGET=rnnoise
ARG BASE_IMAGE=ubuntu:22.04
FROM $BASE_IMAGE as common

SHELL ["/bin/bash", "-c"]

# Build dependencies
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    appmenu-gtk3-module \
    apt-utils \
    autoconf \
    bash \
    build-essential \
    cargo \
    cmake \
    curl \
    g++-10 \
    gcc-10 \
    git \
    libdbus-1-dev \
    libgtk-3-dev \
    librust-openssl-sys-dev \
    libsoup2.4-dev \
    libssl-dev \
    libtool \
    libwebkit2gtk-4.0-dev \
    nodejs \
    npm \
    pkg-config \
    snapd \
    software-properties-common \
    squashfs-tools \
    wget

# Install yarn
RUN npm install -g yarn

# magic-mic deps
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    libpulse-dev libappindicator3-dev libnotify-dev glib2.0 libgtkmm-3.0 libboost-dev
RUN wget http://ftp.gnome.org/pub/gnome/sources/libnotifymm/0.7/libnotifymm-0.7.0.tar.xz && \
    tar xf libnotifymm-0.7.0.tar.xz && \
    cd libnotifymm-0.7.0 && \
    ./configure && \
    make -j 4 install
RUN git clone --depth=1 https://github.com/xiph/rnnoise.git && \
    cd /rnnoise && \
    ./autogen.sh && \
    ./configure && \
    make install -j 4

# install src-web deps
COPY ./src-web /src/src-web
RUN npm --version yarm --version && cd /src/src-web && yarn

# install tauri cli deps
COPY ./package.json /src
RUN cd /src && yarn

# Cargo build
RUN mkdir -p /src/src-tauri/src
COPY ./src-tauri /src
RUN mkdir -p /scr/src-web/build
RUN cd /src/src-tauri && cargo build --release


COPY . /src

# appimagetool seems to want to use fuse for some reason to create the appimage,
# but it doesn't need it. build_appimage.sh checks if fuse is usable with lsmod
# | grep fuse. fuse is annoying to use on docker so we'll just shim lsmod not to
# list fuse. I think the reason it lists fuse even though fuse is not usable on
# docker is probably because of the docker sandboxing
RUN echo -e '!/bin/bash\necho NOTHING' >/usr/local/bin/lsmod && \
    chmod +x /usr/local/bin

FROM common as rnnoise
RUN cd /src && \
    PATH=$HOME/lsmod_shim:$PATH && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_CXX_COMPILER=`which g++-10` \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DAUDIOPROC_CMAKES="$PWD/../src-native/RNNoiseAP.cmake" \
      -DVIRTMIC_ENGINE="PIPESOURCE" .. && \
    make build_tauri -j 4
FROM common as audo
RUN cd /src && \
    PATH=$HOME/lsmod_shim:$PATH && \
    mkdir build && \
    cd build && \
    cmake -C /src-libdenoiser/cache.cmake \
      -DCMAKE_CXX_COMPILER=`which g++-10` \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DAUDIOPROC_CMAKES="$PWD/../src-native/RNNoiseAP.cmake;/src-libdenoiser/AudoAP.cmake" \
      -DVIRTMIC_ENGINE="PIPESOURCE" \
      -DCMAKE_PREFIX_PATH=/src-libdenoiser/cmake_prefix .. && \
    make build_tauri -j 4

# Give target a constant name so that the copy works
FROM $TARGET as build
FROM scratch as bin
COPY --from=build /src/src-tauri/target/release/bundle/appimage/magic-mic_0.1.0_amd64.AppImage .