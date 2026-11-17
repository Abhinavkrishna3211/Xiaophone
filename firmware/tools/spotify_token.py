"""One-time Spotify sign-in for the Xiaophone's cover display (standard library only).

1. developer.spotify.com/dashboard -> Create app. Redirect URI: http://127.0.0.1:8888/callback
   API: Web API. Copy the Client ID.
2. python spotify_token.py <client-id>
3. Log in in the browser that opens, then paste the two printed lines into firmware/src/secrets.h.
"""
import base64, hashlib, http.server, json, secrets, sys, urllib.parse, urllib.request, webbrowser

REDIRECT = "http://127.0.0.1:8888/callback"
client_id = sys.argv[1] if len(sys.argv) > 1 else input("Client ID: ").strip()

verifier = secrets.token_urlsafe(64)
challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
state = secrets.token_urlsafe(16)
auth = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
    "client_id": client_id, "response_type": "code", "redirect_uri": REDIRECT, "state": state,
    "scope": "user-read-currently-playing", "code_challenge_method": "S256", "code_challenge": challenge})

got = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if q.get("state", [""])[0] == state:
            got.update({k: v[0] for k, v in q.items()})
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"Done, you can close this tab." if "code" in got else b"Sign-in failed.")

    def log_message(self, *a):
        pass


print("Opening the browser. If it doesn't open, visit:\n" + auth)
webbrowser.open(auth)
srv = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
while not got:
    srv.handle_request()
if "code" not in got:
    sys.exit("Sign-in failed: " + got.get("error", "unknown"))

body = urllib.parse.urlencode({"grant_type": "authorization_code", "code": got["code"],
                               "redirect_uri": REDIRECT, "client_id": client_id,
                               "code_verifier": verifier}).encode()
req = urllib.request.Request("https://accounts.spotify.com/api/token", body,
                             {"Content-Type": "application/x-www-form-urlencoded"})
tok = json.load(urllib.request.urlopen(req))
print("\nPaste into firmware/src/secrets.h:\n")
print(f'#define SPOTIFY_CLIENT_ID     "{client_id}"')
print(f'#define SPOTIFY_REFRESH_TOKEN "{tok["refresh_token"]}"')
