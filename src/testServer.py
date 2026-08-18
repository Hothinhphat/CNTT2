import paho.mqtt.client as mqtt

BROKER = "localhost"
PORT = 1883
TOPIC = "server/#"

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("connected MQTT Broker")
        client.subscribe(TOPIC)
    else:
        print("connect erorr:", rc)

def on_message(client, userdata, msg):
    topic = msg.topic
    data = msg.payload.decode()

    print(f"Topic: {topic}")
    print(f"Data : {data}")

    if topic == "server/temperature":
        temperature = float(data)
        print(f"-- temprature: {temperature} °C")

    elif topic == "server/humidity":
        humidity = float(data)
        print(f"-- humidity: {humidity} %")

    elif topic == "server/smoke":
        smoke = int(data)
        print(f"-- smoke: {smoke}")

    elif topic == "server/door":
        door = int(data)
        print(f"-- door: {door}")

    print("--------------------")

client = mqtt.Client()

client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER, PORT, 60)
client.loop_forever()