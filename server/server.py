"""
Weather Station Server

This Python script runs as the backend server for an ESP32-based weather station
system. It is designed to run on a Raspberry Pi and communicate with the ESP32
over WiFi using HTTP requests.

System workflow:
1. The ESP32 sends a GET request to retrieve the server's configured location.
2. Then uses that location to query wttr.in for current outdoor temperature.
3. The ESP32 reads its onboard temperature sensor data.
4. Then sends both temperature readings back to this server using a POST request.
5. The server receives, parses, and logs the environmental data for monitoring.
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
from datetime import datetime
import json

SERVER_LOCATION = "Santa+Cruz"

class WeatherStationServer(BaseHTTPRequestHandler):
    def do_GET(self):
        # Returns configured city name for weather lookup
        if self.path == "/location":
            print("\n===== GET /location RECEIVED =====")
            print("Time:", datetime.now())
            print("Sending location:", SERVER_LOCATION)
            print("==================================\n")

            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(SERVER_LOCATION.encode("utf-8"))

        else:
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Not found")

    def do_POST(self):
        # Receives JSON temperature data from ESP32 client
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8")

        print("\n===== POST RECEIVED =====")
        print("Time:", datetime.now())
        print("Path:", self.path)
        print("Raw body:", body)

        try:
            data = json.loads(body)

            print("\n--- Parsed Data ---")
            print("Server location:", data.get("location"))
            print("Outdoor temperature from wttr.in:", data.get("outdoor_temp_c"))
            print("ESP32 sensor temperature:", data.get("esp32_temp_c"))
            print("-------------------")

        except json.JSONDecodeError:
            print("Could not parse JSON.")

        print("=========================\n")

        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"OK")

    def log_message(self, format, *args):
        # Hides default server spam
        return


server = HTTPServer(("0.0.0.0", 1234), WeatherStationServer)

print("Server running on port 1234")
print("Try this from your laptop:")
print("wget http://localhost:1234/location")
print("Waiting for ESP32...")
server.serve_forever()