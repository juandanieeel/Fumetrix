"""Local console only: save the ESP LAN address."""
import getpass
import ipaddress
import json
from pathlib import Path
from werkzeug.security import generate_password_hash

root=Path(__file__).resolve().parent
path=root/'data'/'bridge-config.json'
path.parent.mkdir(exist_ok=True)
old=json.loads(path.read_text()) if path.exists() else {}
default=old.get('esp_ip','192.168.100.206')
ip=input(f'ESP IP [{default}]: ').strip() or default
address=ipaddress.IPv4Address(ip)
if not address.is_private or address.is_loopback or address.is_link_local:
    raise SystemExit('Use the private LAN address shown on the ESP Wi-Fi page.')
old['esp_ip']=str(address)
path.write_text(json.dumps(old,indent=2))
print('ESP address saved. Account access is managed on the Admin page.')
