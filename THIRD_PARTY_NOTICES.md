# Third-party components

## NetFilter SDK

This project uses the user's local NetFilter SDK for Windows demo 1.7.8.4 archive. Header files, import libraries, API DLL and driver remain subject to the NetFilter SDK license. The build extracts them into an ignored build directory and copies required runtime files and the SDK license into the local distribution directory. No SDK source or binary is checked into this project.

Vendor: https://netfiltersdk.com/ — consult the vendor terms before redistribution or production use. The demo connection/socket limit is unchanged.

## JSON for Modern C++

nlohmann/json 3.12.0, Copyright (c) 2013–2025 Niels Lohmann, MIT License.

- Source: https://github.com/nlohmann/json/releases/tag/v3.12.0
- File: `third_party/json.hpp`
- SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
- License: `third_party/json.LICENSE.MIT`

## Architecture references

Netch's ProcessMode/Redirector and the NetFilter SDK SocksRedirector sample were examined for API usage and architecture. The application sources in `src` and tests in `tests` are newly written for this project; Netch sources are not vendored.
