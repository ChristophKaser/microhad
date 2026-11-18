#!/usr/bin/env python3
"""Install the shared capture transport and adapt the pinned Zigbee build tree."""
import re
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
tree = Path(sys.argv[1]) / "crates"


def edit(crate, filename, transform):
    path = tree / crate / "src" / filename
    original = path.read_text()
    changed = transform(original)
    if changed == original:
        raise SystemExit(f"packet capture hook did not match {path}")
    path.write_text(changed)


radio = tree / "ziggurat-phy" / "src"
shutil.copyfile(root / "shared/packet-capture/capture.rs", radio / "gateway_capture.rs")
edit("ziggurat-phy", "lib.rs", lambda text: text + "\nextern crate std;\npub mod gateway_capture;\n")
manifest = tree / "ziggurat-spinel" / "Cargo.toml"
manifest.write_text(manifest.read_text().replace(
    "[dependencies]\n", "[dependencies]\nziggurat-phy.workspace = true\n", 1,
))
edit("ziggurat-server", "main.rs", lambda text: text.replace(
    "    raw_frames: bool,",
    "    raw_frames: bool,\n\n    #[arg(long)]\n    capture_dest: Option<std::net::SocketAddr>,",
).replace(
    "    let args = Args::parse();",
    """    let args = Args::parse();
    let _capture = if args.raw_frames {
        let destination = match args.capture_dest {
            Some(destination) => Some(destination),
            None => std::env::var("PACKET_CAPTURE_DEST").ok()
                .filter(|value| !value.is_empty()).map(|value| value.parse()).transpose()?,
        };
        if destination.is_none() {
            eprintln!("Packet capture disabled because no destination is configured");
        }
        destination.map(|dest| ziggurat_phy::gateway_capture::configure(dest, 1)).transpose()?
    } else {
        None
    };""",
).replace(
    '        if args.raw_frames {\n            filter = filter.with_target("ziggurat_raw", LevelFilter::TRACE);\n        }\n',
    "",
).replace("let mut filter = std::env::var", "let filter = std::env::var"))


def phy(text):
    changes = [
        (
            r'tracing::trace!\(target: "ziggurat_raw",\s*"RX channel=.*?frame\.psdu\);',
            """let mut metadata = [0; 13];
            metadata[0] = frame.channel;
            metadata[1] = frame.rssi as u8;
            metadata[2] = frame.lqi;
            metadata[3..11].copy_from_slice(&frame.timestamp_us.to_le_bytes());
            metadata[11..].copy_from_slice(&(frame.flags as u16).to_le_bytes());
            ziggurat_phy::gateway_capture::record(10, &metadata, &frame.psdu);""",
        ),
        (
            r'tracing::trace!\(target: "ziggurat_raw", "TX channel=.*?frame\.psdu\);',
            'ziggurat_phy::gateway_capture::record(11, &[frame.channel.unwrap_or(0)], &frame.psdu);',
        ),
        (
            r'tracing::trace!\(target: "ziggurat_raw", "TX result.*?frame\.psdu\.get\(2\)\);',
            'ziggurat_phy::gateway_capture::record_debug(12, format_args!("sequence={:?} status={status:?}", frame.psdu.get(2)));',
        ),
    ]
    for pattern, replacement in changes:
        text, count = re.subn(pattern, replacement, text, flags=re.S)
        if count != 1:
            raise SystemExit("radio capture hook did not match")
    return text


edit("ziggurat-phy-spinel", "lib.rs", phy)


def spinel(text):
    for direction, kind in (("RX", 13), ("TX", 14)):
        text = re.sub(
            rf'tracing::trace!\(target: "ziggurat_raw",\s*"SPINEL_{direction} .*?frame\.payload\);',
            f"""let mut metadata = [0; 6];
                                metadata[0] = frame.header.network_link_id;
                                metadata[1] = frame.header.transaction_id;
                                metadata[2..].copy_from_slice(&(frame.command_id as u32).to_le_bytes());
                                ziggurat_phy::gateway_capture::record({kind}, &metadata, &frame.payload);""",
            text, flags=re.S,
        )
    text = text.replace(
        'tracing::info!(target: "radio_trace", "{message}");',
        'ziggurat_phy::gateway_capture::record(17, &[], message.as_bytes());',
    )
    return text.replace(
        'tracing::trace!("Sending frame {frame:?}");',
        'ziggurat_phy::gateway_capture::record_debug(15, format_args!("Sending frame {frame:?}"));',
    )


edit("ziggurat-spinel", "client.rs", spinel)
edit("ziggurat-spinel", "lib.rs", lambda text: text.replace(
    'tracing::trace!("RX: {header:?} {command_id:?} {payload:02X?}");',
    """let mut metadata = [0; 6];
        metadata[0] = header.network_link_id;
        metadata[1] = header.transaction_id;
        metadata[2..].copy_from_slice(&(command_id as u32).to_le_bytes());
        ziggurat_phy::gateway_capture::record(18, &metadata, payload);""",
))

frame_dump = re.compile(
    r'tracing::(?:trace|debug)!\(\s*"(?:Decrypted frame|Received (?:APS data|802\.15\.4(?: command)?) frame|Sending 802\.15\.4 frame|Prepared (?:unicast ZDP|unicast|broadcast|group broadcast) APS frame)[^"]*"[^;]*?\);',
    re.S,
)
driver = tree / "ziggurat-driver" / "src"
for path in (driver / "zigbee_stack.rs", *(driver / "zigbee_stack").glob("*.rs")):
    original = path.read_text()
    changed = frame_dump.sub(
        lambda match: 'ziggurat_phy::gateway_capture::record_debug(16, format_args!('
        + match.group()[match.group().index('"'):-2] + '));',
        original,
    )
    if changed != original:
        path.write_text(changed)
