# Chrono Rift — Operating Systems Project

## Overview

**Chrono Rift** is a multi-threaded Operating Systems simulation project developed in C++.
The project demonstrates core OS concepts including:

* Process/thread management
* Scheduling algorithms
* Inter-process communication
* Synchronization mechanisms
* Deadlock detection and handling
* Resource management
* Real-time simulation

The system is divided into three major components:

* **HIP (Human Interaction Process)**
* **ASP (Autonomous System Process)**
* **Arbiter (System Controller)**

These components work together to simulate a controlled operating environment.

---

# Project Architecture

```
                 +----------------+
                 |     HIP        |
                 | Player Process |
                 +-------+--------+
                         |
                         |
                 +-------v--------+
                 |      ASP       |
                 | NPC Simulation |
                 +-------+--------+
                         |
                         |
                 +-------v--------+
                 |    Arbiter     |
                 | OS Controller  |
                 +----------------+
```

---

# Components

## 1. HIP (Human Interaction Process)

Location:

```
/hip
```

Responsible for:

* Handling user input
* Managing player threads
* Player-side simulation
* User interaction interface

Files:

```
hip.cpp
hip_tui.cpp
input.cpp
player_threads.cpp
```

---

## 2. ASP (Autonomous System Process)

Location:

```
/asp
```

Responsible for:

* NPC management
* Autonomous behavior
* Thread execution
* Simulation logic

Files:

```
asp.cpp
npc_logic.cpp
npc_threads.cpp
asp_tui.cpp
```

---

## 3. Arbiter

Location:

```
/arbiter
```

Responsible for:

* System coordination
* Scheduling
* Deadlock management
* Resource control
* Process monitoring

Files:

```
arbiter.cpp
scheduler.cpp
deadlock.cpp
signals.cpp
render.cpp
```

---

# Requirements

## Software Requirements

* C++ Compiler
* Make
* Linux environment recommended

Required packages:

```bash
g++
make
```

---

# Build Instructions

Clone the repository:

```bash
git clone https://github.com/Ibtisam313/OS_Project.git
```

Navigate into project:

```bash
cd OS_Project
```

Compile the project:

```bash
make
```

The generated binaries will be placed inside:

```
bin/
```

---

# Running the Project

Run each component separately:

## Start Arbiter

```bash
./bin/arbiter
```

## Start ASP

```bash
./bin/asp
```

## Start HIP

```bash
./bin/hip
```

---

# Docker Support

Build Docker image:

```bash
docker build -t chrono-rift .
```

Run container:

```bash
docker run -it chrono-rift
```

---

# Project Structure

```
Chrono_Rift
│
├── arbiter
│   ├── scheduler.cpp
│   ├── deadlock.cpp
│   └── arbiter.cpp
│
├── asp
│   ├── npc_logic.cpp
│   ├── npc_threads.cpp
│   └── asp.cpp
│
├── hip
│   ├── input.cpp
│   ├── player_threads.cpp
│   └── hip.cpp
│
├── shared.cpp
├── shared.h
├── Makefile
├── Dockerfile
└── requirements.txt
```

---

# Concepts Implemented

## Multithreading

The project uses threads to simulate:

* Players
* NPCs
* System processes

---

## Scheduling

The Arbiter manages execution using scheduling logic.

---

## Deadlock Handling

The system detects and manages resource conflicts.

---

## Synchronization

Shared resources are controlled using synchronization mechanisms.

---

# Authors

**Ibtisam**

Operating Systems Project

---

# License

This project is developed for educational purposes.
