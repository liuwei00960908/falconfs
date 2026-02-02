Name:           falconfs
Version:        0.1.0
Release:        1%{?dist}
Summary:        FalconFS is a high-performance distributed file system (DFS) optimized for AI workloads.

License:        Apache-2.0
URL:            https://gitee.com/openeuler/FalconFS.git
Source0:        %{name}-%{version}.tar.gz

%global brpc_version 1.14.1
%global prometheus_version 1.3.0
%global zk_version 3.9.1
%global spdlog_version 1.12.0
%global obs_version 3.24.12

Source1:        brpc-%{brpc_version}.tar.gz
Source2:        prometheus-cpp-with-submodules-%{prometheus_version}.tar.gz
Source3:        apache-zookeeper-%{zk_version}.tar.gz
Source4:        spdlog-%{spdlog_version}.tar.gz
Source5:        huaweicloud-sdk-c-obs-%{obs_version}.tar.gz

BuildRequires:  bash gcc gcc-c++ make
BuildRequires:  cmake ninja-build
BuildRequires:  autoconf automake libtool
BuildRequires:  bison flex readline-devel
BuildRequires:  openssl-devel
BuildRequires:  gflags-devel glog-devel leveldb-devel snappy-devel
BuildRequires:  fmt-devel
BuildRequires:  gperftools-devel libunwind-devel
BuildRequires:  rdma-core-devel
BuildRequires:  fuse-devel
BuildRequires:  libcurl-devel
BuildRequires:  jansson-devel libffi-devel
BuildRequires:  libzstd-devel xz-devel expat-devel
BuildRequires:  libxml2-devel
BuildRequires:  systemd-devel
BuildRequires:  protobuf-devel protobuf-compiler
BuildRequires:  flatbuffers-devel flatbuffers-compiler
BuildRequires:  jsoncpp-devel thrift-devel
BuildRequires:  cppunit-devel
BuildRequires:  gtest-devel gmock-devel
BuildRequires:  python3-devel
BuildRequires:  maven java-11-openjdk-devel
BuildRequires:  wget tar rsync
BuildRequires:  libstdc++-static libpq-devel zstd-devel
BuildRequires:  perl
BuildRequires:  chrpath

%description
FalconFS is a high-performance distributed file system.
It integrates seamlessly with cloud environments.

%global _debugsource_packages 0
%global debug_package %{nil}

%prep
%setup -q
echo "Extracting dependencies to BUILD root..."
tar -xf %{SOURCE1} -C ..
tar -xf %{SOURCE2} -C ..
tar -xf %{SOURCE3} -C ..
tar -xf %{SOURCE4} -C ..
tar -xf %{SOURCE5} -C ..
cd ..
cd %{name}-%{version} || cd %{name}

%build
set -euo pipefail
export DEPS_PREFIX="%{_builddir}/falconfs-deps"
export STAGE_ROOT="%{_builddir}/falconfs-stage"
export FALCONFS_INSTALL_DIR="${STAGE_ROOT}/usr/local/falconfs"
mkdir -p "${DEPS_PREFIX}" "${STAGE_ROOT}"

cd "%{_builddir}/brpc-%{brpc_version}"
mkdir -p build && cd build
cmake -GNinja \
      -DWITH_GLOG=ON \
      -DWITH_RDMA=ON \
      -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}/usr/local" \
      -DCMAKE_PREFIX_PATH="${DEPS_PREFIX}/usr/local" \
      -DCMAKE_CXX_FLAGS="-Wno-error -Wno-deprecated-declarations" \
      ..
ninja -j%{?_smp_build_ncpus}
ninja install

cd "%{_builddir}/prometheus-cpp-with-submodules"
mkdir -p build && cd build
cmake .. \
      -DBUILD_SHARED_LIBS=ON \
      -DENABLE_PULL=ON \
      -DENABLE_COMPRESSION=OFF \
      -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}/usr/local"
make -j%{?_smp_build_ncpus}
make install

cd "%{_builddir}/apache-zookeeper-%{zk_version}"
mvn compile -DskipTests -pl zookeeper-jute -T 1C
cd zookeeper-client/zookeeper-client-c
autoreconf -if
./configure --prefix="${DEPS_PREFIX}/usr/local"
make -j%{?_smp_build_ncpus}
make install

cd "%{_builddir}/spdlog-%{spdlog_version}"
mkdir -p build && cd build
cmake .. \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}/usr/local"
make -j%{?_smp_build_ncpus}
make install

cd "%{_builddir}/huaweicloud-sdk-c-obs-%{obs_version}/source/eSDK_OBS_API/eSDK_OBS_API_C++"
if [ "$(uname -m)" = "aarch64" ]; then PLAT="arm"; bash build_aarch.sh sdk; else PLAT="linux"; bash build.sh sdk; fi
mkdir -p "${DEPS_PREFIX}/usr/local/obs"
tar -zxf sdk.tgz -C "${DEPS_PREFIX}/usr/local/obs"
INTERNAL_SPDLOG="%{_builddir}/huaweicloud-sdk-c-obs-%{obs_version}/build/script/Provider/build/${PLAT}/spdlog-%{spdlog_version}/lib"
cp -d "${INTERNAL_SPDLOG}"/libspdlog.so* "${DEPS_PREFIX}/usr/local/obs/lib/"
ln -sf /usr/local/obs/lib/libiconv.so "${DEPS_PREFIX}/usr/local/obs/lib/libiconv.so.0" 2>/dev/null || true
rm -f "${DEPS_PREFIX}/usr/local/obs/lib/libcurl.so"* || true

export OBS_INSTALL_PREFIX="/usr/local/obs"
export CPLUS_INCLUDE_PATH="${DEPS_PREFIX}/usr/local/obs/include:${DEPS_PREFIX}/usr/local/include:${CPLUS_INCLUDE_PATH:-}"
export C_INCLUDE_PATH="${DEPS_PREFIX}/usr/local/obs/include:${DEPS_PREFIX}/usr/local/include:${C_INCLUDE_PATH:-}"
export LD_LIBRARY_PATH="${DEPS_PREFIX}/usr/local/obs/lib:${DEPS_PREFIX}/usr/local/lib64:${DEPS_PREFIX}/usr/local/lib:${LD_LIBRARY_PATH:-}"
export LIBRARY_PATH="${DEPS_PREFIX}/usr/local/lib64:${DEPS_PREFIX}/usr/local/lib:${LIBRARY_PATH:-}"
export CMAKE_PREFIX_PATH="${DEPS_PREFIX}/usr/local:${CMAKE_PREFIX_PATH:-}"
export PKG_CONFIG_PATH="${DEPS_PREFIX}/usr/local/lib64/pkgconfig:${DEPS_PREFIX}/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

cd "%{_builddir}/%{name}-%{version}"
export PG_INSTALL_DIR="${FALCONFS_INSTALL_DIR}/falcon_metadb"
cd third_party/postgres
CFLAGS="-O2" ./configure --prefix="${PG_INSTALL_DIR}" --without-icu --enable-rpath \
    LDFLAGS="-Wl,-rpath,${FALCONFS_INSTALL_DIR}/lib64:${FALCONFS_INSTALL_DIR}/lib"
perl src/backend/utils/generate-errcodes.pl --outfile src/backend/utils/errcodes.h src/backend/utils/errcodes.txt
mkdir -p src/include/utils
rm -f src/include/utils/errcodes.h
cp -f src/backend/utils/errcodes.h src/include/utils/errcodes.h
make -C src/backend generated-headers
make -C src/backend/nodes node-support-stamp
mkdir -p src/include/nodes
rm -f src/include/nodes/nodetags.h
cp -f src/backend/nodes/nodetags.h src/include/nodes/nodetags.h
make -j%{?_smp_build_ncpus}
cd contrib && make -j%{?_smp_build_ncpus}
cd "%{_builddir}/%{name}-%{version}"
bash build.sh install pg
bash build.sh build falcon
bash build.sh install falcon

%install
set -euo pipefail
export DEPS_PREFIX="%{_builddir}/falconfs-deps"
export STAGE_ROOT="%{_builddir}/falconfs-stage"

rm -rf "%{buildroot}"
mkdir -p "%{buildroot}"
cp -a "${STAGE_ROOT}/"* "%{buildroot}/"
mkdir -p "%{buildroot}/usr/local/lib" "%{buildroot}/usr/local/lib64" "%{buildroot}/usr/local/obs"

if [ -d "${DEPS_PREFIX}/usr/local/lib" ]; then
    mkdir -p "%{buildroot}/usr/local"
    cp -a "${DEPS_PREFIX}/usr/local/lib" "%{buildroot}/usr/local/"
fi
if [ -d "${DEPS_PREFIX}/usr/local/lib64" ]; then
    mkdir -p "%{buildroot}/usr/local"
    cp -a "${DEPS_PREFIX}/usr/local/lib64" "%{buildroot}/usr/local/"
fi
if [ -d "${DEPS_PREFIX}/usr/local/obs" ]; then
    mkdir -p "%{buildroot}/usr/local"
    cp -a "${DEPS_PREFIX}/usr/local/obs" "%{buildroot}/usr/local/"
fi

mkdir -p "%{buildroot}%{_sysconfdir}/ld.so.conf.d"
cat > "%{buildroot}%{_sysconfdir}/ld.so.conf.d/local.conf" <<'EOF'
/usr/local/lib
/usr/local/lib64
EOF
cat > "%{buildroot}%{_sysconfdir}/ld.so.conf.d/obs.conf" <<'EOF'
/usr/local/obs/lib
EOF
cat > "%{buildroot}%{_sysconfdir}/ld.so.conf.d/falconfs.conf" <<'EOF'
/usr/local/falconfs/lib64
/usr/local/falconfs/lib
/usr/local/falconfs/falcon_metadb/lib
EOF

mkdir -p "%{buildroot}%{_sysconfdir}/profile.d"
cat > "%{buildroot}%{_sysconfdir}/profile.d/falconfs.sh" <<'EOF'
export PATH=/usr/local/falconfs/falcon_metadb/bin:/usr/local/falconfs/bin:$PATH
export FALCONFS_WORKSPACE=/var/lib/falconfs
export PGUSER=falconMeta
EOF

# Strip non-standard RPATH/RUNPATH to satisfy rpmbuild checks
find "%{buildroot}" -type f -exec chrpath -d {} \; 2>/dev/null || true

%pre
getent group falconMeta >/dev/null || groupadd -r falconMeta
getent passwd falconMeta >/dev/null || \
    useradd -r -g falconMeta -d /var/lib/falconfs -s /sbin/nologin falconMeta
exit 0

%post
/sbin/ldconfig
mkdir -p /usr/local/falconfs/falcon
ln -sf /usr/local/falconfs/falcon_metadb/lib/postgresql/libbrpcplugin.so \
    /usr/local/falconfs/falcon/libbrpcplugin.so
mkdir -p /var/lib/falconfs/data || true
chown -R falconMeta:falconMeta /var/lib/falconfs/data || true

%postun
/sbin/ldconfig

%files
/usr/local/falconfs/
/usr/local/lib/*
/usr/local/lib64/*
/usr/local/obs/
%config(noreplace) %{_sysconfdir}/ld.so.conf.d/local.conf
%config(noreplace) %{_sysconfdir}/ld.so.conf.d/obs.conf
%config(noreplace) %{_sysconfdir}/ld.so.conf.d/falconfs.conf
%{_sysconfdir}/profile.d/falconfs.sh
