# Zidane Watcher — Detailed Guide

Complete reference for the SenseCap Watcher firmware, MCP integration, and OpenClaw setup.

## Table of Contents

1. [Project Overview](#project-overview)
2. [Hardware](#hardware)
3. [Firmware Features](#firmware-features)
   - [Agent States & Animations](#agent-states--animations)
   - [Sprite System](#sprite-system)
   - [Background Images](#background-images)
   - [Dialog System](#dialog-system)
   - [Display Sleep](#display-sleep)
   - [Knob Controls](#knob-controls)
   - [RGB LED](#rgb-led)
   - [Voice Input (Push-to-Talk)](#voice-input-push-to-talk)
   - [Speaker Output (TTS)](#speaker-output-tts)
   - [Himax AI Camera](#himax-ai-camera)
   - [Person Detection & Presence](#person-detection--presence)
   - [Gesture Recognition](#gesture-recognition)
   - [OpenClaw Heartbeat](#openclaw-heartbeat)
   - [Web UI](#web-ui)
4. [Sprite & Background Planning Dashboard](#sprite--background-planning-dashboard)
   - [Running the Dashboard](#running-the-dashboard)
   - [Main Preview](#main-preview)
   - [Sprite Catalog](#sprite-catalog)
   - [Background Catalog](#background-catalog)
5. [REST API Reference](#rest-api-reference)
6. [MCP Server](#mcp-server)
   - [Tools](#tools)
   - [Resources](#resources)
7. [Watcher Daemon](#watcher-daemon)
   - [What It Does](#what-it-does)
   - [Message Queue](#message-queue)
   - [Voice Pipeline](#voice-pipeline)
   - [Presence Notifications](#presence-notifications)
8. [OpenClaw Integration](#openclaw-integration)
   - [MCP Configuration](#mcp-configuration)
   - [Agent Events](#agent-events)
9. [Building & Flashing](#building--flashing)
   - [Build from /tmp](#build-from-tmp)
   - [sdkconfig Rules](#sdkconfig-rules)
   - [Flash Commands](#flash-commands)
   - [Serial Monitor](#serial-monitor)
10. [SD Card Setup](#sd-card-setup)
    - [Sprite Assets](#sprite-assets)
    - [Himax Firmware](#himax-firmware)
11. [Customization](#customization)
    - [Adding Sprites](#adding-sprites)
    - [Changing Backgrounds](#changing-backgrounds)
    - [Voice Configuration](#voice-configuration)
12. [Troubleshooting](#troubleshooting)
13. [Architecture Diagram](#architecture-diagram)

---

## Project Overview

The Zidane Watcher turns a SenseCap Watcher device into a physical AI desk companion for OpenClaw. It displays an animated Zidane (Final Fantasy IX) character on a 412×412 round LCD screen, detects people and gestures via a Himax AI camera, accepts voice input through a built-in microphone, and speaks responses through its speaker using Piper TTS.

The system has three components:

| Component | Runs on | Role |
|---|---|---|
| **Firmware** (`pokewatcher/`) | ESP32-S3 (Watcher device) | Display rendering, camera AI, web server, voice recording, hardware control |
| **MCP Server** (`watcher-mcp/src/index.ts`) | Mac (spawned by OpenClaw) | Stateless stdio bridge — 7 tools for OpenClaw to control the Watcher |
| **Daemon** (`watcher-mcp/src/daemon.ts`) | Mac (LaunchAgent, 24/7) | Polls Watcher every 5s for voice audio, presence changes, message dismissals. Owns the message queue. Sends events to OpenClaw. |

Communication: Mac ↔ Watcher over HTTP (port 80). MCP Server ↔ Daemon over localhost HTTP (port 8378). Daemon → OpenClaw via `openclaw agent` CLI.

## Hardware

The SenseCap Watcher is an ESP32-S3 device with these peripherals:

| Peripheral | Interface | Details |
|---|---|---|
| LCD Display | SPI (HSPI) | 412×412 round, ST7701 controller |
| Himax WE2 Camera | SPI2 (VSPI) | AI chip with onboard models, 12 MHz SPI clock |
| Microphone | I2S | 16kHz/16-bit mono, PSRAM buffer |
| Speaker | I2S + codec | 16kHz/16-bit mono PCM playback |
| Rotary Knob | GPIO (encoder + button) | Press, double-press, long-press, rotate |
| RGB LED | GPIO | Programmable color, used for state indication |
| SD Card | SPI2 (shared with Himax) | FAT32, powered off after boot to free SPI bus |
| WiFi | Built-in ESP32-S3 | 2.4GHz, used for REST API and web UI |
| IO Expander | I2C | Controls power to LCD, camera, SD card |
| Battery | ADC | Optional LiPo, not used in desk mode |
