from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter


W = 1280
H = 720
OUT = Path(__file__).resolve().parent

COLORS = {
    "bg": "#000000",
    "panel": "#101820",
    "header": "#0a1a30",
    "accent": "#00E5FF",
    "text": "#FFFFFF",
    "dim": "#808080",
    "faint": "#444444",
    "good": "#00FF00",
    "warn": "#FFA500",
    "bad": "#FF0000",
    "bubble_in": "#0a3040",
    "bubble_out": "#0a3010",
    "selected": "#1a2a3a",
    "cursor": "#FFE000",
}


def font(size: int, bold: bool = False):
    candidates = [
        "C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


F_TITLE = font(38, True)
F_H1 = font(24, True)
F_H2 = font(18, True)
F_BODY = font(16, False)
F_SMALL = font(13, False)


def rounded(draw: ImageDraw.ImageDraw, box, radius=12, fill=None, outline=None, width=1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def chip(draw, x, y, text, color):
    bbox = draw.textbbox((0, 0), text, font=F_SMALL)
    w = bbox[2] - bbox[0] + 18
    rounded(draw, (x, y, x + w, y + 26), radius=13, fill=color)
    draw.text((x + 9, y + 5), text, fill=COLORS["bg"], font=F_SMALL)


def panel(draw, x, y, w, h, title):
    rounded(draw, (x, y, x + w, y + h), radius=14, fill=COLORS["panel"], outline=COLORS["faint"])
    draw.text((x + 16, y + 10), title, fill=COLORS["accent"], font=F_H2)


def stats_card(draw, x, y, w, h, value, label, color):
    rounded(draw, (x, y, x + w, y + h), radius=10, fill="#0b131d", outline=COLORS["faint"])
    draw.text((x + 16, y + 12), value, fill=color, font=font(28, True))
    draw.text((x + 16, y + 48), label, fill=COLORS["dim"], font=F_SMALL)


def draw_header(draw, title, status):
    rounded(draw, (24, 20, W - 24, 66), radius=10, fill=COLORS["header"])
    draw.text((44, 31), title, fill=COLORS["accent"], font=F_H1)
    draw.text((W - 180, 33), status, fill=COLORS["good"], font=F_BODY)


def draw_caption(base: Image.Image, headline: str, subline: str):
    overlay = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    rounded(draw, (34, H - 150, W - 34, H - 34), radius=20, fill=(3, 10, 20, 220))
    draw.text((60, H - 128), headline, fill=COLORS["text"], font=F_TITLE)
    draw.text((60, H - 84), subline, fill=COLORS["accent"], font=F_BODY)
    return Image.alpha_composite(base.convert("RGBA"), overlay).convert("RGB")


def add_glow(img: Image.Image):
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)
    draw.ellipse((930, 50, 1250, 370), fill=(0, 229, 255, 36))
    draw.ellipse((20, 420, 420, 780), fill=(0, 255, 0, 18))
    glow = glow.filter(ImageFilter.GaussianBlur(55))
    return Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")


def base_canvas():
    img = Image.new("RGB", (W, H), COLORS["bg"])
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 0, W, H), fill=COLORS["bg"])
    draw.line((70, 110, W - 70, 110), fill="#08111a", width=2)
    return img, draw


def gateway_client_frame():
    img, draw = base_canvas()
    draw_header(draw, "TiggyOpenMesh Gateway Client", "Connected")

    panel(draw, 40, 90, 1200, 96, "Connection")
    chip(draw, 64, 135, "COM7", COLORS["accent"])
    chip(draw, 156, 135, "Hub Online", COLORS["good"])
    chip(draw, 286, 135, "AES Enabled", COLORS["warn"])
    draw.text((400, 139), "Field Gateway Alpha", fill=COLORS["text"], font=F_BODY)

    panel(draw, 40, 204, 560, 224, "Mesh Topology")
    hub = (160, 315)
    nodes = [(330, 250), (438, 317), (334, 390), (505, 242), (515, 382)]
    for node in nodes:
        draw.line((hub[0], hub[1], node[0], node[1]), fill=COLORS["accent"], width=3)
    draw.ellipse((hub[0] - 26, hub[1] - 26, hub[0] + 26, hub[1] + 26), fill=COLORS["good"])
    draw.text((132, 346), "GW", fill=COLORS["text"], font=F_SMALL)
    for i, node in enumerate(nodes, 1):
        draw.ellipse((node[0] - 18, node[1] - 18, node[0] + 18, node[1] + 18), fill="#153a4b", outline=COLORS["accent"], width=2)
        draw.text((node[0] - 12, node[1] + 22), f"{i:02d}", fill=COLORS["text"], font=F_SMALL)

    panel(draw, 40, 446, 560, 110, "Gateway Stats")
    stats_card(draw, 58, 484, 120, 56, "128", "Radio In", COLORS["accent"])
    stats_card(draw, 192, 484, 120, 56, "126", "Peer Out", COLORS["good"])
    stats_card(draw, 326, 484, 120, 56, "7", "Nodes", COLORS["warn"])
    stats_card(draw, 460, 484, 120, 56, "Live", "Hub", COLORS["good"])

    panel(draw, 624, 204, 616, 352, "Discovered Nodes")
    rows = [
        ("Node 0041", "-62 dBm", "Direct", COLORS["good"]),
        ("Node 006A", "-74 dBm", "1 hop via 0041", COLORS["accent"]),
        ("Node 00AF", "-81 dBm", "2 hop via 006A", COLORS["warn"]),
        ("Node 0132", "-68 dBm", "Direct", COLORS["good"]),
        ("Node 0199", "-88 dBm", "2 hop via 00AF", COLORS["bad"]),
    ]
    y = 238
    for name, rssi, route, color in rows:
        rounded(draw, (642, y, 1220, y + 52), radius=8, fill="#0b131d")
        draw.text((660, y + 10), "●", fill=color, font=F_H2)
        draw.text((684, y + 10), name, fill=COLORS["text"], font=F_BODY)
        draw.text((920, y + 10), rssi, fill=color, font=F_BODY)
        draw.text((1040, y + 10), route, fill=COLORS["dim"], font=F_SMALL)
        y += 60

    return draw_caption(add_glow(img), "Live network view from the gateway", "Topology, node health, and link quality update in one place.")


def gateway_packets_frame():
    img, draw = base_canvas()
    draw_header(draw, "TiggyOpenMesh Gateway Client", "Inspecting Traffic")

    panel(draw, 40, 90, 1200, 210, "Packet Waterfall")
    packet_rows = [
        ("RX", "0041 -> FFFF  TTL3  TEMP=21.8C", COLORS["bubble_in"]),
        ("TX", "GW -> HUB  forward  seq 1842", COLORS["bubble_out"]),
        ("RX", "006A -> 0041  TTL2  RELAY OK", COLORS["bubble_in"]),
        ("RX", "00AF -> FFFF  TTL1  DOOR=OPEN", COLORS["bubble_in"]),
        ("TX", "GW -> HUB  forward  seq 1845", COLORS["bubble_out"]),
    ]
    y = 128
    for direction, text, fill in packet_rows:
        rounded(draw, (62, y, 1216, y + 26), radius=6, fill=fill)
        draw.text((76, y + 5), direction, fill=COLORS["accent"], font=F_SMALL)
        draw.text((126, y + 5), text, fill=COLORS["text"], font=F_SMALL)
        y += 34

    panel(draw, 40, 318, 660, 212, "Packet Inspector")
    inspector_lines = [
        "Source: 00AF",
        "Destination: FFFF",
        "Sequence: 1845",
        "RSSI: -81 dBm",
        "Payload: SDATA|door=open|battery=91",
        "Decrypt: success",
    ]
    y = 354
    for line in inspector_lines:
        draw.text((64, y), line, fill=COLORS["text"], font=F_BODY)
        y += 28

    panel(draw, 722, 318, 518, 212, "Sensor Monitor")
    bars = [50, 72, 33, 88, 64]
    labels = ["Temp", "Humidity", "Battery", "RSSI", "Light"]
    for i, value in enumerate(bars):
        x = 756 + i * 90
        draw.rectangle((x, 468 - value, x + 44, 468), fill=COLORS["accent"])
        draw.text((x - 2, 478), labels[i], fill=COLORS["dim"], font=F_SMALL)
        draw.text((x + 4, 442 - value), str(value), fill=COLORS["text"], font=F_SMALL)

    panel(draw, 40, 548, 1200, 130, "Benefit")
    draw.text((64, 590), "Troubleshoot mesh traffic fast with packet-level visibility and built-in decryption.", fill=COLORS["text"], font=font(22, True))

    return draw_caption(add_glow(img), "Every packet is visible", "Waterfall and inspector views turn field debugging into a desktop task.")


def logic_builder_frame():
    img, draw = base_canvas()
    draw_header(draw, "TiggyOpenMesh Gateway Client", "Automation Ready")

    panel(draw, 40, 90, 1200, 584, "Logic Builder")
    chip(draw, 64, 130, "Rule: Freeze Alert", COLORS["accent"])
    chip(draw, 224, 130, "Enabled", COLORS["good"])
    chip(draw, 332, 130, "Check: 5s", COLORS["warn"])
    chip(draw, 438, 130, "Gap: 30s", COLORS["warn"])
    chip(draw, 1110, 130, "Deploy", COLORS["good"])

    blocks = [
        ((120, 240, 300, 300), "#18334a", "Sensor Input", "temperature < 2C"),
        ((380, 240, 560, 300), "#2b2b14", "Condition", "battery > 20%"),
        ((650, 240, 850, 300), "#10371a", "Action", "send alert"),
        ((930, 240, 1130, 300), "#341414", "Output", "relay siren"),
    ]
    for box, fill, title, body in blocks:
        rounded(draw, box, radius=16, fill=fill, outline=COLORS["accent"])
        draw.text((box[0] + 16, box[1] + 18), title, fill=COLORS["accent"], font=F_H2)
        draw.text((box[0] + 16, box[1] + 58), body, fill=COLORS["text"], font=F_BODY)
    for x1, y1, x2, y2 in [(300, 270, 380, 270), (560, 270, 650, 270), (850, 270, 930, 270)]:
        draw.line((x1, y1, x2, y2), fill=COLORS["cursor"], width=5)
        draw.polygon([(x2, y2), (x2 - 12, y2 - 8), (x2 - 12, y2 + 8)], fill=COLORS["cursor"])

    rounded(draw, (120, 360, 1130, 560), radius=12, fill="#07111a")
    draw.text((144, 388), "Event log:", fill=COLORS["accent"], font=F_H2)
    logs = [
        "14:37:12  Rule matched on node 00AF",
        "14:37:12  Alert packet queued for hub",
        "14:37:13  Local output toggled for siren",
        "14:37:16  Cooldown active",
    ]
    y = 428
    for log in logs:
        draw.text((144, y), log, fill=COLORS["text"], font=F_BODY)
        y += 34

    return draw_caption(add_glow(img), "Visual automation at the edge", "Build rules from live sensor events without writing custom control logic.")


def hub_dashboard_frame():
    img, draw = base_canvas()
    draw_header(draw, "TiggyOpenMesh Gateway Hub", "Running")

    panel(draw, 40, 90, 1200, 74, "Hub Control")
    chip(draw, 64, 118, "Port 8765", COLORS["accent"])
    chip(draw, 172, 118, "Auth Key", COLORS["good"])
    chip(draw, 278, 118, "AES Ready", COLORS["warn"])

    stats_card(draw, 40, 186, 170, 96, "18,442", "Packets Relayed", COLORS["accent"])
    stats_card(draw, 226, 186, 170, 96, "4", "Gateways Online", COLORS["good"])
    stats_card(draw, 412, 186, 170, 96, "12", "Connections", COLORS["warn"])

    panel(draw, 604, 186, 636, 244, "Connected Gateways")
    rows = [
        ("North Ridge", "A12F4C9E", "3h 12m", "4821"),
        ("Harbor West", "B91D7E24", "2h 44m", "4379"),
        ("Farm South", "C2258A01", "58m 10s", "2655"),
        ("Depot East", "DF9910AC", "17m 42s", "940"),
    ]
    y = 224
    for name, gid, uptime, packets in rows:
        rounded(draw, (624, y, 1218, y + 42), radius=8, fill="#0b131d")
        draw.text((642, y + 10), "●", fill=COLORS["good"], font=F_H2)
        draw.text((668, y + 11), name, fill=COLORS["text"], font=F_BODY)
        draw.text((856, y + 11), gid[:8], fill=COLORS["dim"], font=F_SMALL)
        draw.text((980, y + 11), uptime, fill=COLORS["dim"], font=F_SMALL)
        draw.text((1110, y + 11), packets, fill=COLORS["accent"], font=F_SMALL)
        y += 50

    panel(draw, 40, 302, 540, 198, "Cross-Island Packet Flow")
    hubs = [(120, 400), (250, 350), (310, 456), (460, 388)]
    for node in hubs:
        draw.line((120, 400, node[0], node[1]), fill=COLORS["accent"], width=3)
        draw.ellipse((node[0] - 20, node[1] - 20, node[0] + 20, node[1] + 20), fill="#123245", outline=COLORS["accent"])
    draw.ellipse((96, 376, 144, 424), fill=COLORS["good"])
    for x in [180, 210, 360]:
        draw.ellipse((x, 390, x + 12, 402), fill=COLORS["cursor"])

    panel(draw, 40, 520, 540, 158, "Sensor Monitor")
    for i, (name, value) in enumerate([("Temp", "21.8C"), ("Humidity", "47%"), ("Door", "Open"), ("Battery", "91%")]):
        x = 64 + i * 120
        rounded(draw, (x, 562, x + 96, 636), radius=10, fill="#0b131d")
        draw.text((x + 14, 580), value, fill=COLORS["text"], font=F_BODY)
        draw.text((x + 14, 606), name, fill=COLORS["dim"], font=F_SMALL)

    panel(draw, 604, 448, 636, 230, "Packet Log")
    log_lines = [
        "[14:37:12] relay packet from North Ridge to 3 peers",
        "[14:37:13] decrypted SDATA from node 00AF",
        "[14:37:15] Harbor West acknowledged forward",
        "[14:37:17] Farm South registered sensor update",
        "[14:37:18] Depot East joined hub",
    ]
    y = 486
    for line in log_lines:
        draw.text((626, y), line, fill=COLORS["text"], font=F_SMALL)
        y += 34

    return draw_caption(add_glow(img), "One hub, multiple gateways", "Track live relay traffic and gateway health across the whole deployment.")


def final_frame():
    img, draw = base_canvas()
    draw_header(draw, "TiggyOpenMesh", "Demo Preview")
    draw.text((68, 130), "Live mesh visibility, packet inspection, and automation control.", fill=COLORS["text"], font=font(30, True))
    draw.text((68, 176), "Mocked from the current Gateway Client and Gateway Hub GUI layout.", fill=COLORS["accent"], font=F_BODY)

    thumbs = [
        gateway_client_frame().resize((360, 202)),
        gateway_packets_frame().resize((360, 202)),
        logic_builder_frame().resize((360, 202)),
        hub_dashboard_frame().resize((360, 202)),
    ]
    positions = [(72, 250), (458, 250), (72, 478), (458, 478)]
    for thumb, (x, y) in zip(thumbs, positions):
        img.paste(thumb, (x, y))
        rounded(draw, (x, y, x + 360, y + 202), radius=14, outline=COLORS["accent"], width=2)

    rounded(draw, (848, 250, 1190, 680), radius=18, fill=COLORS["panel"])
    draw.text((878, 290), "Benefits", fill=COLORS["accent"], font=font(28, True))
    bullets = [
        "See node health live",
        "Inspect and decrypt packets",
        "Track multi-gateway relays",
        "Build local automation rules",
        "Explain the system visually",
    ]
    y = 346
    for item in bullets:
        draw.text((878, y), f"+ {item}", fill=COLORS["text"], font=F_BODY)
        y += 54

    return draw_caption(add_glow(img), "Draft promo mock", "This is a generated preview. Real screenshots can replace any frame later.")


FRAMES = [
    ("01_gateway_client.png", gateway_client_frame),
    ("02_gateway_packets.png", gateway_packets_frame),
    ("03_logic_builder.png", logic_builder_frame),
    ("04_hub_dashboard.png", hub_dashboard_frame),
    ("05_final.png", final_frame),
]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    images = []
    for filename, fn in FRAMES:
        image = fn()
        path = OUT / filename
        image.save(path)
        images.append(image)

    gif_frames = []
    for image in images:
        gif_frames.extend([image] * 2)
    gif_frames[0].save(
        OUT / "tiggyopenmesh_mock_preview.gif",
        save_all=True,
        append_images=gif_frames[1:],
        duration=1100,
        loop=0,
    )


if __name__ == "__main__":
    main()
