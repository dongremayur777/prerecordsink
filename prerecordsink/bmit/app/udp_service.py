import socket
import json
import time

config_file_path = "/opt/bmit/app/configs/"
device_info = None


def get_config():
    print("Populating configurations")
    global device_info
    try:
        with open(config_file_path + "device_info.json", "r") as f:
            device_info = json.load(f)
    except Exception as e:
        print("Config file error ", e, flush=True)


def get_version():
    try:
        with open(config_file_path + "device.version", "r") as file:
            version = file.read().strip()
    except FileNotFoundError:
        version = "1.0.0"
    return version


def start_listner():
    global device_info
    listen_port = 10080  # Port number to listen on
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", listen_port))

    print(f"Listening on port {listen_port}")
    mac_address = get_mac_address()
    model_id = get_model_id()
    
    # Loop indefinitely, waiting for messages
    while True:
        # Receive a message
        data, address = sock.recvfrom(1024)
        message = data.decode("utf-8")
        get_config()
        user_id = ""
        device_name = get_device_name()
        dock_id = get_dock_id()
        device_id= get_device_id()

        other_info = {}

        if device_info and "info" in device_info:
            other_info = device_info["info"]

        if other_info:
            if "user_id" in other_info:
                user_id = other_info["user_id"]

        device = {
            "name": str(model_id),
            "mac": mac_address,
            "device_id": device_id,
            "user_id": user_id,
            "battery": 55,
            "port": 5009,
            "model_id": model_id,
            "device_name": device_name,
            "dock_id":dock_id
        }

        # Respond to the message
        response = "HELLO-BM-" + json.dumps(device, separators=(",", ":"))
        response_port = 10081  # Port number to respond on
        response_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        response_sock.sendto(response.encode("utf-8"), (address[0], response_port))
        response_sock.close()
        print(f'Responded to {message} with "{response}"')


def get_mac_address():
    mac_address = None
    try:
        with open(config_file_path + "device.mac", "r") as file:
            mac_address = file.read().strip()
    except Exception as e:
        print("Config file error ", e)
    return mac_address


def get_model_id():
    model_id = None
    try:
        with open(config_file_path + "model.id", "r") as file:
            model_id = file.read().strip()
    except Exception as e:
        print("Config file error ", e)
    return model_id
   
def get_device_name():
    device_name = None
    try:
        with open(config_file_path + "device.name", "r") as file:
            device_name = file.read().strip()
    except Exception as e:
        print("device name file error ", e)
    return device_name
   
def get_dock_id():
    dock_id = None
    try:
        with open(config_file_path + "dock.id", "r") as file:
            dock_id = file.read().strip()
    except Exception as e:
        print("dock_id file error ", e)
    return dock_id

def get_device_id():
    device_id = None
    try:
        with open(config_file_path + "device.id", "r") as file:
            device_id = file.read().strip()
    except Exception as e:
        print("device_id file error ", e)
    return device_id
        
def get_battery_info():
    battery = None
    try:
        with open(config_file_path + "battery.info", "r") as file:
            battery = file.read().strip()
    except Exception as e:
        print("Config file error ", e)
    return battery



start_listner()
