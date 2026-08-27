#include <iostream>
#include <string>
#include <cstdio>
#include <sstream>
#include <iomanip>

// sudo apt install libusb-1.0-0-dev
#include <libusb-1.0/libusb.h>

#define VID 0x054c
#define PS4_PID 0x05c4


// 現在のBluetooth master addressを取得
void show_master(libusb_device_handle* handle, int interface_number, int pid){
  int value = (pid == PS4_PID)? 0x0312 : 0x03f5;
  unsigned char msg[50] = {};
  int res = libusb_control_transfer(
    handle,
    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
    0x01,               // bRequest
    value,              // wValue
    interface_number,   // wIndex
    msg,
    sizeof(msg),
    5000                // timeout [ms]
  );

  if(res < 0){
    fprintf(stderr, "libusb_control_transfer failed: %s\n", libusb_error_name(res));
    return;
  }

  if(pid == PS4_PID){
    printf("Current Bluetooth master: %02x:%02x:%02x:%02x:%02x:%02x\n", msg[15], msg[14], msg[13], msg[12], msg[11], msg[10]);
  }
}

// MACアドレスを書き込む
int write_ps4(
  libusb_device_handle* handle,
  int interface_number,
  int pid,
  const int mac[6]
){
  if(pid != PS4_PID){
    std::cerr << "Not a PS4 controller.\n";
    return -1;
  }

  printf("Setting master bd_addr to %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  /*
   * sixpair.cのPS4用データをそのまま使用
   *
   * MACアドレスは逆順で格納する。
   */
  unsigned char msg[23] = {
    0x13,
    static_cast<unsigned char>(mac[5]),
    static_cast<unsigned char>(mac[4]),
    static_cast<unsigned char>(mac[3]),
    static_cast<unsigned char>(mac[2]),
    static_cast<unsigned char>(mac[1]),
    static_cast<unsigned char>(mac[0]),
    0x56,
    0xE8,
    0x81,
    0x38,
    0x08,
    0x06,
    0x51,
    0x41,
    0xC0,
    0x7F,
    0x12,
    0xAA,
    0xD9,
    0x66,
    0x3C,
    0xCE
  };

  int res = libusb_control_transfer(
    handle,
    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
    0x09,               // bRequest
    0x0313,             // wValue
    interface_number,   // wIndex
    msg,
    sizeof(msg),
    5000
  );

  if(res < 0){
    fprintf(stderr, "SET_MASTER failed: %s\n", libusb_error_name(res));
    return res;
  }

  if(res != static_cast<int>(sizeof(msg))){
    std::cerr << "Unexpected transferred size: " << res << " bytes\n";
    return -1;
  }

  std::cout << "Bluetooth master address set successfully.\n";
  return res;
}


// PS4コントローラーに接続
int connect_ps4(libusb_device* device, const libusb_device_descriptor& desc, int interface_number, const int mac[6]){
  libusb_device_handle* handle = nullptr;
  int res = libusb_open(device, &handle);
  if(res != 0){
    std::cerr << "libusb_open failed: " << libusb_error_name(res)<< '\n';
    return res;
  }
  
  // LinuxのHIDドライバがInterfaceを使用している場合に切り離す
  bool kernel_driver_detached = false;

  int active = libusb_kernel_driver_active(
    handle,
    interface_number
  );

  if(active == 1){
    std::cout << "Kernel HID driver is active. Detaching...\n";
    res = libusb_detach_kernel_driver(
      handle,
      interface_number
    );

    if(res != 0){
      std::cerr << "libusb_detach_kernel_driver failed: " << libusb_error_name(res)<< '\n';
      libusb_close(handle);
      return res;
    }

    kernel_driver_detached = true;
  }else if(active < 0){
    std::cerr << "libusb_kernel_driver_active failed: " << libusb_error_name(active)<< '\n';
    libusb_close(handle);
    return active;
  }

  // Interfaceを確保
  res = libusb_claim_interface(handle, interface_number);

  if(res != 0){
    std::cerr << "libusb_claim_interface failed: " << libusb_error_name(res)<< '\n';

    if(kernel_driver_detached){
      libusb_attach_kernel_driver(handle, interface_number);
    }

    libusb_close(handle);
    return res;
  }

  // 現在のBluetooth MACを取得
  show_master(handle, interface_number, desc.idProduct);

  // 新しいMACアドレスを書き込む
  res = write_ps4(handle, interface_number, desc.idProduct, mac);

  if(res < 0){
    std::cerr << "Failed to set Bluetooth master address.\n";
  }

  // Interfaceを解放
  int release_res = libusb_release_interface(handle, interface_number);

  if(release_res != 0){
    std::cerr << "libusb_release_interface failed: " << libusb_error_name(release_res)<< '\n';
  }

  // kernel driverを再接続
  if(kernel_driver_detached){
    int attach_res = libusb_attach_kernel_driver(handle, interface_number);
    if(attach_res != 0){
      std::cerr << "libusb_attach_kernel_driver failed: " << libusb_error_name(attach_res)<< '\n';
    }
  }

  libusb_close(handle);
  return res;
}


int main(){
  libusb_context* ctx = nullptr;
  
  // libusb初期化
  int res = libusb_init(&ctx);

  if(res != 0){
    std::cerr << "libusb_init failed: " << libusb_error_name(res) << '\n';
    return 1;
  }

  // デバイス一覧取得
  libusb_device** devices = nullptr;

  ssize_t count = libusb_get_device_list(ctx, &devices);

  if(count < 0){
    std::cerr << "Failed to get device list: " << libusb_error_name(count) << '\n';
    libusb_exit(ctx);
    return 1;
  }

  // PS4コントローラーを探す
  libusb_device* controller = nullptr;
  libusb_device_descriptor controller_desc{};

  bool isFoundController = false;

  for(ssize_t i = 0; i < count; i++){
    libusb_device* device = devices[i];
    libusb_device_descriptor desc{};

    if(libusb_get_device_descriptor(device, &desc) != 0)continue;

    if(desc.idVendor == VID && desc.idProduct == PS4_PID){
      std::cout << "PS4コントローラーが見つかりました。\n";

       controller = device;
      controller_desc = desc;
      isFoundController = true;
        break;
    }
  }

  if(!isFoundController){
    std::cout << "PS4コントローラーが見つかりませんでした。" << '\n';
    libusb_free_device_list(devices, 1);
    libusb_exit(ctx);
    return 1;
  }

  // Configuration Descriptorを取得
  libusb_config_descriptor* config = nullptr;

  res = libusb_get_active_config_descriptor(controller, &config);

  if(res != 0){
    std::cerr << "Failed to get active configuration: " << libusb_error_name(res)<< '\n';

    libusb_free_device_list(devices, 1);
    libusb_exit(ctx);

    return 1;
  }

  // HID Interfaceを探す
  int interface_number = -1;

  for(int i = 0; i < config->bNumInterfaces; i++){
    const libusb_interface& interface_ = config->interface[i];

    for(int j = 0; j < interface_.num_altsetting; j++){
      const libusb_interface_descriptor& alt = interface_.altsetting[j];
      // HID = Interface Class 3
      if(alt.bInterfaceClass == 3){
        interface_number = alt.bInterfaceNumber;
        break;
      }
    }
    if(interface_number != -1){
      break;
    }
  }

  if(interface_number == -1){
    std::cerr << "HID interface not found.\n";

    libusb_free_config_descriptor(config);
    libusb_free_device_list(devices, 1);

    libusb_exit(ctx);
    return 1;
  }

  std::cout << "HID interface number: " << interface_number << '\n';

  /*
  * テスト用MACアドレス
  *
  * ここは実際に設定したいBluetoothアダプター等の
  * BD_ADDRに変更する。
  */
  const int mac[6] = {
     0x00,
    0x11,
    0x22,
    0x33,
    0x44,
    0x56
  };

  // PS4に接続して読み出し・書き込み
  connect_ps4(
    controller,
    controller_desc,
    interface_number,
    mac
  );

  // 後処理
  libusb_free_config_descriptor(config);
  libusb_free_device_list(devices, 1);
  libusb_exit(ctx);
  return 0;
}