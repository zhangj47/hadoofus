Installing libhadoofus
======================

#### Bare minimum

```sh
export CFLAGS="your favorite cc flags"
make all
sudo make install
```

#### Configurable make variables

Aside from `CFLAGS`, you can configure `PREFIX`, `LIBDIR`, and `INCLUDEDIR`.
These default to `/usr/local`, `$PREFIX/lib`, and `$PREFIX/include`,
respectively. For distribution packaging, you can also set `DISTDIR` to an
alternative root directory (you probably also want to set `PREFIX` to `/usr`).

#### Running post-build tests

Note, this requires a live HDFS server at `namenode.example.com`.

```sh
HDFS_TEST_NODE_ADDRESS=namenode.example.com make test
```