<p align="center">
  <img src="CodeIssueService.png" width="60%" alt="CODEISSUESERVICE-logo">
</p>
<p align="center">
    <h1 align="center">CODEISSUESERVICE</h1>
</p>
<p align="center">
    <em>Streamlining Issue Management with Seamless Integration and Automation.</em>
</p>
<p align="center">
	<img src="https://img.shields.io/github/license/theesfeld/CodeIssueService?style=flat-square&logo=opensourceinitiative&logoColor=white&color=4fa800" alt="license">
	<img src="https://img.shields.io/github/last-commit/theesfeld/CodeIssueService?style=flat-square&logo=git&logoColor=white&color=4fa800" alt="last-commit">
	<img src="https://img.shields.io/github/languages/top/theesfeld/CodeIssueService?style=flat-square&color=4fa800" alt="repo-top-language">
	<img src="https://img.shields.io/github/languages/count/theesfeld/CodeIssueService?style=flat-square&color=4fa800" alt="repo-language-count">
</p>
<p align="center">
		<em>Built with the tools and technologies:</em>
</p>
<p align="center">
	<img src="https://img.shields.io/badge/YAML-CB171E.svg?style=flat-square&logo=YAML&logoColor=white" alt="YAML">
	<img src="https://img.shields.io/badge/C-A8B9CC.svg?style=flat-square&logo=C&logoColor=black" alt="C">
</p>

<br>

<details><summary>Table of Contents</summary>

- [ Overview](#-overview)
- [ Features](#-features)
- [ Repository Structure](#-repository-structure)
- [ Modules](#-modules)
- [ Getting Started](#-getting-started)
    - [ Prerequisites](#-prerequisites)
    - [ Installation](#-installation)
    - [ Usage](#-usage)
    - [ Tests](#-tests)
- [ Project Roadmap](#-project-roadmap)
- [ Contributing](#-contributing)
- [ License](#-license)
- [ Acknowledgments](#-acknowledgments)

</details>
<hr>

##  Overview

This starts an httpd service on port 8080 (configurable). Included is a .yaml file for github actions.
when an issue is opened in the repository, the httpd service is notified, and the following occurs:

1) The repository is cloned locally
2) The issue and repository are scanned by AI
3) Code is modified / generated to satisfy the issue
4) The updated code is reviewed by AI for breaking changes
5) a PR is created and pushed to the repository for manual review

Config file: /etc/code_issue_service.conf
Systemd unit file: /etc/systemd/system/code_issue_service.service

Issue will queue in the redis cache, and are solved one at a time.



** PR ARE VERY VERY VERY WELCOME **
** THIS IS A WORK IN PROGRESS **

---

##  Features

|    |   Feature         | Description |
|----|-------------------|---------------------------------------------------------------|
| ⚙️  | **Architecture**  | The project uses a multi-threaded C application with a service daemon setup to handle GitHub issues, parse JSON, and interact with web services. |
| 🔩 | **Code Quality**  | Code integrates well-known libraries and follows a structured format with clear modular separation for ease of understanding and maintenance. |
| 📄 | **Documentation** | Basic documentation is provided through comments in the code and configuration files, covering setup and operational details. |
| 🔌 | **Integrations**  | Directly integrates with GitHub for issue tracking and external APIs using `curl`. It also interacts with Redis, possibly for caching. |
| 🧩 | **Modularity**    | The project's use of separate configuration files and a structured approach in the `src` directory suggests a modular design. |
| 🧪 | **Testing**       | There is no explicit mention of a testing framework in the provided details, suggesting a potential area for improvement. |
| ⚡️  | **Performance**   | Utilizes `pthread` for concurrent operations and `microhttpd` for HTTP services, potentially offering good performance under load. |
| 🛡️ | **Security**      | Employs syscalls for logging and signal handling but lacks detailed documentation on specific security protocols or data protection measures. |
| 📦 | **Dependencies**  | Key libraries include `cJSON`, `curl`, `git2`, `hiredis`, `microhttpd`, and system-level integrations through `pthread` and `syslog`. |
| 🚀 | **Scalability**   | The use of multi-threading and external service integrations suggest scalability, but specific scaling strategies are not documented. |
```

---

##  Repository Structure

```sh
└── CodeIssueService/
    ├── LICENSE
    ├── Makefile
    ├── code_issue_service.conf
    ├── code_issue_service.service
    ├── issue_listener.yaml
    └── src
        └── code_issue_service.c
```

---

##  Modules

<details closed><summary>.</summary>

| File | Summary |
| --- | --- |
| [Makefile](https://github.com/theesfeld/CodeIssueService/blob/main/Makefile) | Facilitates the compilation, installation, and management of the CodeIssueService application, including the setup of necessary user accounts, service integration, and configuration file placements within a systems architecture, ensuring secure and organized deployment and maintenance of the service. |
| [code_issue_service.service](https://github.com/theesfeld/CodeIssueService/blob/main/code_issue_service.service) | Defines the operational settings for the Code Issue Service, ensuring it starts after network availability, with continuous runtime management under specified user environments. It logs outputs to syslog and retrieves necessary API keys for function, integrating smoothly into system boot processes via multi-user targets. |
| [issue_listener.yaml](https://github.com/theesfeld/CodeIssueService/blob/main/issue_listener.yaml) | Issue Listener configures GitHub Actions to trigger notifications when new issues are opened. It specifies a job that runs on Ubuntu and uses a CURL command to post issue details to a server, integrating GitHub repository events with external systems for enhanced issue tracking and response. |
| [code_issue_service.conf](https://github.com/theesfeld/CodeIssueService/blob/main/code_issue_service.conf) | Configures the CodeIssueService by setting server parameters, GitHub integration, AI model usage, and defining prompts for issue analysis, code generation, review processes, and pull request creation within the software systems architecture. |

</details>

<details closed><summary>src</summary>

| File | Summary |
| --- | --- |
| [code_issue_service.c](https://github.com/theesfeld/CodeIssueService/blob/main/src/code_issue_service.c) | Integration with External ServicesThe use of libraries like `cJSON`, `curl`, `git2`, `hiredis`, and others indicates interactions with web services (likely for fetching data from APIs), parsing JSON, managing git repositories, and possibly caching or storing data in Redis.2. **HTTP Server CapabilitiesThe inclusion of `microhttpd` suggests that this service can also operate as an HTTP server, possibly to receive webhook events directly from code repositories or serve its status.3. **Multi-threading SupportUsage of `pthread` for handling multiple threads, which is crucial for performance when dealing with I/O operations or long-wait operations such as network requests or large computational tasks.4. **Configuration and LoggingConfiguration management with `ini` files, alongside advanced logging capabilities managed by syscalls (`syslog`), which are essential for maintaining a robust, manageable service.5. **Signal Handling and Long-running Service MaintenanceImplements signal handling (`signal.h`) and continuously running service characteristics (likely facilitated by the daemon-like properties inferred from the `.service` file in the repository).This file likely represents the core operational logic of the CodeIssueService, managing both the configuration, execution, and the interfacing between the services functional components and external systems. It’s responsible for the real-time monitoring and processing of issues in software projects, leveraging the repository |

</details>

---

##  Getting Started

###  Prerequisites

#### Core:

**gcc**: `version 14.2.1`

#### Libraries:

**cJSON**
**curl**
**git2**
**hiredis**
**microhttpd**
**pthread**

### Services:

Redis or Valkey server

###  Installation

Build the project from source:

1. Clone the CodeIssueService repository:
```sh
❯ git clone https://github.com/theesfeld/CodeIssueService
```

2. Navigate to the project directory:
```sh
❯ cd CodeIssueService
```

3. Build:
```sh
❯ make
> sudo make install
```

###  Usage

To run the project, edit the configuration file:

```sh
> sudo vim /etc/code_issue_service.conf
```

Then enable/start the service:

```sh
> sudo systemctl enable --now code_issue_service.service
```

Output will be sent to syslog

---

##  Contributing

Contributions are welcome! Here are several ways you can contribute:

- **[Report Issues](https://github.com/theesfeld/CodeIssueService/issues)**: Submit bugs found or log feature requests for the `CodeIssueService` project.
- **[Submit Pull Requests](https://github.com/theesfeld/CodeIssueService/blob/main/CONTRIBUTING.md)**: Review open PRs, and submit your own PRs.
- **[Join the Discussions](https://github.com/theesfeld/CodeIssueService/discussions)**: Share your insights, provide feedback, or ask questions.

<details closed>
<summary>Contributing Guidelines</summary>

1. **Fork the Repository**: Start by forking the project repository to your github account.
2. **Clone Locally**: Clone the forked repository to your local machine using a git client.
   ```sh
   git clone https://github.com/theesfeld/CodeIssueService
   ```
3. **Create a New Branch**: Always work on a new branch, giving it a descriptive name.
   ```sh
   git checkout -b new-feature-x
   ```
4. **Make Your Changes**: Develop and test your changes locally.
5. **Commit Your Changes**: Commit with a clear message describing your updates.
   ```sh
   git commit -m 'Implemented new feature x.'
   ```
6. **Push to github**: Push the changes to your forked repository.
   ```sh
   git push origin new-feature-x
   ```
7. **Submit a Pull Request**: Create a PR against the original project repository. Clearly describe the changes and their motivations.
8. **Review**: Once your PR is reviewed and approved, it will be merged into the main branch. Congratulations on your contribution!
</details>

<details closed>
<summary>Contributor Graph</summary>
<br>
<p align="left">
   <a href="https://github.com{/theesfeld/CodeIssueService/}graphs/contributors">
      <img src="https://contrib.rocks/image?repo=theesfeld/CodeIssueService">
   </a>
</p>
</details>

---

##  License

GPL 3.0
