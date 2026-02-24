import time
import json
import threading
import paho.mqtt.client as mqtt
import firebase_admin
from firebase_admin import credentials
from firebase_admin import db

MQTT_BROKER = "localhost"  # ten kod działa na serwerze z lokalnym brokerem MQTT
MQTT_PORT = 1883

# Ścieżka do pliku pobranego z Firebase Console
FIREBASE_CRED_PATH = "admin_sdk.json"
FIREBASE_DB_URL = (
    "https://iot-anti-theft-976fb-default-rtdb.europe-west1.firebasedatabase.app/"
)

# init Firebase
cred = credentials.Certificate(FIREBASE_CRED_PATH)
firebase_admin.initialize_app(cred, {"databaseURL": FIREBASE_DB_URL})


def get_uid_by_mac(mac):

    ref = db.reference(f"devices_registry/{mac}")
    uid = ref.get()

    if uid:
        print(f"🔍 Znaleziono właściciela dla {mac}: {uid}")
    else:
        print(f"⚠️ Urządzenie {mac} nie jest przypisane do nikogo!")
        try:
            topic = f"iot/device/{mac}/config"
            payload = {"cmd": "FACTORY_RESET"}
            mqtt_client.publish(topic, json.dumps(payload))
            print(f"📤 Wysłano FACTORY_RESET do {mac}")
        except Exception as e:
            print(f"❌ Nie udało się wysłać FACTORY_RESET do {mac}: {e}")

    return uid


def on_connect(client, userdata, flags, rc):
    print(f"✅ Połączono z MQTT (Kod: {rc})")
    client.subscribe("iot/device/+/data")  # Zmieniłem na + żeby łapał wszystkie MACi


def on_message(client, userdata, msg):

    # Odbiera dane z ESP32 i zapisuje do Firebase
    try:
        topic = msg.topic
        payload_str = msg.payload.decode()
        data = json.loads(payload_str)

        # Wyciągnij MAC z tematu: iot/device/MAC_ADRES/data
        # topic.split('/') -> ['iot', 'device', 'MAC', 'data']
        mac = topic.split("/")[2]

        cmd = data.get("status")
        if cmd == "DELETE_DB":
            uid = get_uid_by_mac(mac)
            if not uid:
                print(f"Ignorowanie DELETE_DB z nieprzypisanego urządzenia {mac}")
                return
            db.reference(f"users/{uid}/devices/{mac}").delete()
            db.reference(f"devices_registry/{mac}").delete()
            print(f"🗑️ Usunięto urządzenie {mac} z bazy dla UID {uid}")
            return

        uid = get_uid_by_mac(mac)
        if not uid:
            print(f"Ignorowanie danych z nieprzypisanego urządzenia {mac}")
            return  # Ignoruj nieprzypisane urządzenia

        # zapisz pomiar (ostatni odczyt)
        # ścieżka: users/{uid}/devices/{mac}/measurements
        measurements_ref = db.reference(f"users/{uid}/devices/{mac}/measurements")

        # Tworzymy nowy rekord z timestampem serwera
        measurements_ref.push(
            {
                "accel_x": data.get("x", 0),
                "accel_y": data.get("y", 0),
                "accel_z": data.get("z", 0),
                "battery_mv": data.get("battery_mv", 0),
                "timestamp": {".sv": "timestamp"},  # Firebase Server Time
            }
        )
        print(f"📥 Otrzymano dane z {mac}: {data}")

        # Aktualizacja ostatniego napięcia baterii (jeśli przyszło w danych)
        if "battery_mv" in data:
            db.reference(f"users/{uid}/devices/{mac}/config").update(
                {"battery_mv": data.get("battery_mv", 0)}
            )

        # Aktualizacja stanu urządzenia z danych ESP
        status = data.get("status")
        if status in ("DISARMED", "ARMED", "ROBBERY"):
            db.reference(f"users/{uid}/devices/{mac}/config").update({"state": status})
            if status == "ROBBERY":
                print(f"🚨 ALARM! Urządzenie {mac} zgłasza kradzież!")

    except Exception as e:
        print(f"❌ Błąd przetwarzania MQTT: {e}")


def send_mqtt_config(mac, config_data):
    # Wysyła konfigurację do ESP32

    topic = f"iot/device/{mac}/config"

    # Tłumaczenie formatu Firebase na format ESP32
    payload = {}

    # Obsługa stanu (ARMED/DISARMED)
    if "state" in config_data:
        if config_data["state"] == "ARMED":
            payload["cmd"] = "ARM"
        elif config_data["state"] == "DISARMED":
            payload["cmd"] = "DISARM"
        elif config_data["state"] == "ROBBERY":
            payload["cmd"] = "ROBBERY"

        # Jeśli ROBBERY, to nic nie wysyłamy do ESP, bo to stan odczytu

    # if config_data.get("alarm") is True:
    #     payload["cmd"] = "ALARM_BY_SERVER"

    # Obsługa progu czułości
    if "threshold" in config_data:
        payload["threshold"] = float(config_data["threshold"])

    if payload:
        # QoS=1 + retain, żeby komendy ARM/DISARM miały najwyższy priorytet dostarczenia
        # i były natychmiast dostępne po reconnect.
        mqtt_client.publish(topic, json.dumps(payload), qos=1, retain=True)
        print(f"Wysłano config do {mac}: {payload}")


# callback Firebase (SERVER -> ESP)
def on_firebase_change(event):
    """
    Nasłuchuje zmian w całej gałęzi 'users', ale reaguje TYLKO
    na zmiany w węźle 'config' konkretnego urządzenia.
    """
    try:
        if event.data is None:
            return
        if event.path == "/":
            return

        # Ścieżka zdarzenia wygląda np. tak: /UID/devices/MAC/config/threshold
        # Dzielimy po "/" i usuwamy puste elementy
        path_parts = event.path.strip("/").split("/")

        # Musimy mieć co najmniej: UID, devices, MAC, config (długość 4)
        if len(path_parts) < 4:
            return

        # Sprawdzamy strukturę
        # 0: UID, 1: devices, 2: MAC, 3: "config" lub "measurements"

        if path_parts[1] != "devices":
            return

        # KLUCZOWE: Reagujemy tylko jeśli zmieniono 'config'
        # Ignorujemy zmiany w 'measurements' (żeby nie robić pętli zwrotnej)
        if path_parts[3] != "config":
            return

        # Wyciągamy dane
        uid = path_parts[0]
        mac = path_parts[2]

        print(f"🔄 Wykryto zmianę w ustawieniach dla: {mac}")

        # Pobieramy PEŁNĄ aktualną konfigurację z bazy, żeby wysłać spójny stan
        full_config = db.reference(f"users/{uid}/devices/{mac}/config").get()

        if full_config:
            send_mqtt_config(mac, full_config)

    except Exception as e:
        print(f"⚠️ Błąd listenera Firebase: {e}")


# hardbeat serwera MQTT
def server_heartbeat_loop():

    # wysyła heartbeat co 5 sekund
    while True:
        mqtt_client.publish("iot/server/status", "online")

        time.sleep(5)


# konfiguracja MQTT
mqtt_client = mqtt.Client()

mqtt_client.on_connect = on_connect  # Callback połączenia (odpala się po reconnect)
mqtt_client.on_message = on_message  # Callback wiadomości (przychodzące dane z ESP)

try:
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
except Exception as e:
    print(f"Nie można połączyć z MQTT: {e}")
    exit(1)


# basłuchujemy zmian w węźle users, żeby wyłapać kliknięcia w config
db.reference("users").listen(on_firebase_change)

# 3. Uruchomienie wątku Heartbeat
hb_thread = threading.Thread(target=server_heartbeat_loop)
hb_thread.daemon = True
hb_thread.start()


print("Serwer Backendowy uruchomiony...")
mqtt_client.loop_forever()  # ogarnia callbacki MQTT
