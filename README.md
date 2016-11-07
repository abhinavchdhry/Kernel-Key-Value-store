## In-Kernel Key Value Store
### Completed as part of project work for CSC 501 - Operating Systems

#### How to build:
1. In kernel_module directory, do: `sudo make` followed by `sudo make install`. This will create the loadable kernel module *keyvalue.ko* in the directory
2. In library directory do: `make` followed by  `make install`. This will load the keyvalue user library into the appropriate location
3. In benchmark directory do: `make`. This will create the binaries *benchmark* and *validate* for running the benchmark tests
