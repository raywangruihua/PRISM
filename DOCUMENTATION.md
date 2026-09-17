# Documentation

This document describes the setup and steps required to recreate the experiment.

The project source code file structure is as show below:

```
PRISM
│       
└───prism
    │   
    ├───prism_optee # OP-TEE TA source code
    │   │   Android.mk
    │   │   CMakeLists.txt
    │   │   Makefile
    │   │   
    │   └───ta
    │       │   Android.mk
    │       │   Makefile
    │       │   prism_ta.c
    │       │   sub.mk
    │       │   user_ta_header_defines.h
    │       │   
    │       └───include
    │               prism_ta.h
    │               
    └───prism_ros2_ws # ROS2 source code
        └───src
            └───prism
                │   CMakeLists.txt
                │   LICENSE
                │   package.xml
                │   
                ├───include
                │   └───prism
                │           tee_raii.hpp
                │           
                └───src
                        decrypted_publisher.cpp # CA source
                        encrypted_publisher.cpp
```

## Setup

For this project, we used a Yahboom ROSMASTER X3 with Jetson Orin Nano, a second Jetson Orin Nano acting as the encryption hardware middleman, and an Orbbec Astra Pro Plus camera.

The overall data pipeline is as follows: 

1. Orbbec Astra Pro Plus camera captures frame data
2. Frame data is transferred to hardware middleman Jetson via USB protocol
3. Orbbec ROS2 wrapper node publishes frames to `/camera/color/image_raw`
4. `encrypted_publisher` subscribes to `/camera/color/image_raw`, encrypts the frames via AES GCM 128 and publishes it to `/camera/color/image_raw/encrypted`
5. `decrypted_publisher` subscribes to `/camera/color/image_raw/encrypted` via cross-machine ROS2 network
6. `decrypted_publisher` calls TA to decrypt frame and process frame
7. `decrypted_publisher` publishes final frame to `/camera/color/image_raw/decrypted`

In hindsight, the hardware middleman should be connected to the X3 via USB instead of the ROS2 network, as it exposes the raw image topic as well. But an advantage of using this setup during testing is that we can easily view all topics on a central machine via `rviz2`. Expect to see a gray-scaled version of the original raw frame when viewing `/camera/color/image_raw/decrypted` via `rviz2`. An easy extension of this experiment would be to rewrite `encrypted_publisher` to send encrypted frames over serial to `decrypted_publisher` using the python and the `pyserial` module.

### Setup Encryption Middleman (Jetson Orin Nano)

1. Flash the device with Jetpack 6.X, developer software packages not required
2. Compile or download the Orbbec SDKv1 prebuilt binaries from this [repo](https://github.com/orbbec/OrbbecSDK/tree/main)
3. Download ROS2 Humble according to this [guide](https://docs.ros.org/en/humble/Installation/Ubuntu-Install-Debs.html)
4. Create a ROS2 workspace
4. Download the Orbbec ROS2 wrapper package from this [repo](https://github.com/orbbec/OrbbecSDK_ROS2/tree/main) into the workspace (make sure to clone the `main` branch instead of `v2-main`)
5. Transfer the `prism` ROS2 package from `$PROJECT_ROOT/prism/prism_ros2_ws/src` to the workspace
6. Compile the workspace with `colcon build`
7. Launch the camera using the command shown in the Orbbec ROS2 wrapper repo and run the `encryption_publisher` node

Verify all ROS2 nodes are running via `ros2 topic list` or `rviz2`. The rest of the setup pertains to the X3.

### Compile OP-TEE TA

Compile the TA via the command below:

```bash
make -C /path/to/prism_optee \
     CROSS_COMPILE="" \
     TA_DEV_KIT_DIR="/home/jetson/jetson-public-srcs/Linux_for_Tegra/source/jetson-optee-srcs/optee/build/t234/export-ta_arm64" \
     OPTEE_CLIENT_EXPORT="/home/jetson/jetson-public-srcs/Linux_for_Tegra/source/jetson-optee-srcs/optee/install/t234/usr" \
     TEEC_EXPORT="/home/jetson/jetson-public-srcs/Linux_for_Tegra/source/jetson-optee-srcs/optee/install/t234/usr" \
     -j"$(nproc)"

cp $PROJECT_ROOT/prism/prism_optee/ta/prism_ta.o /lib/optee_armtz
```

`TA_DEV_DIT_DIR` and `TEEC_EXPORT` may differ for your device, the ones shown are specific to the Jetpack SDK image. Typically, OP-TEE software is compiled across a host and a device but is compiled directly on the device in this case. The TA UUID defined in `prism/prims_optee/ta/include/prism_ta.h` is set to a default value, ensure that the UUID set does not clash with any pre-existing TAs on your devices, as the newly compiled TA will overwrite the old TA. After compilation, copy the compiled `prism_ta.o` file to `/lib/optee_armtz`.

### Run ROS2 nodes

Compile and run the `decrypted_publisher` node. If you are not familar with ROS2, refer to this [guide](https://docs.ros.org/en/foxy/Tutorials/Beginner-Client-Libraries/Colcon-Tutorial.html#build-the-workspace). Make sure that the hardware middleman Jetson and the X3 Jetson are connected over the same network, and `ROS_DOMAIN_ID` is the same on both (do `EXPORT ROS_DOMAIN_ID=$DOMAIN_ID` if they are different).

#### Permission Denied for ROS2 CA

If you run into this error when trying to run `decrypted_publisher`, it is because the node/process does not have permission to access the TEE. To resolve this, we must give root access to the node, however it is not possible to run ROS2 commands with the `sudo` prefix. It is also not reccommended to run ROS2 with root permissions. A workaround this is to give the user access to the TEE device as seen below:

```bash
sudo bash -c 'echo "KERNEL==\"tee[0-9]*\", MODE=\"0666\"" > /etc/udev/rules.d/99-tee.rules'
sudo udevadm control --reload-rules
sudo udevadm trigger
```

This grants all users read and write access to the TEE for testing purposes. Access should only be allowed for intended users only in production.
