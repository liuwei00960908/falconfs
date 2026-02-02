# Dependencies

```
yum install -y rpmdevtools
```

# Prepare sources

Place the following tarballs in `~/rpmbuild/SOURCES/`:

- `falconfs-0.1.0.tar.gz` (project source)
- `brpc-1.14.1.tar.gz`
- `prometheus-cpp-with-submodules-1.3.0.tar.gz`
- `apache-zookeeper-3.9.1.tar.gz`
- `spdlog-1.12.0.tar.gz`
- `huaweicloud-sdk-c-obs-3.24.12.tar.gz`

# Build RPM (scheme 2: full source build)

```
rpmdev-setuptree
cp falconfs.source.spec ~/rpmbuild/SPECS/
rpmbuild -ba ~/rpmbuild/SPECS/falconfs.source.spec
ls -alh ~/rpmbuild/RPMS/*/falconfs-0.1.0-1.*.rpm
```

# Install

```
yum localinstall falconfs-0.1.0-1.*.rpm
```
