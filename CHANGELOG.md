# Changelog

All notable changes to the PokéWatcher firmware will be documented in this file.

## [Unreleased]

### Fixed
- **#1 Dual queue consumers**: mood_engine_task and coordinator_task both consumed from the same FreeRTOS queue, causing ~50% of events to be delivered to the wrong consumer. Removed coordinator_task; mood engine now dispatches all event types via callbacks.
