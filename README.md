# Ryan FC RoboCup 2D Team

Final-year project focused on developing and evaluating a custom RoboCup Soccer 2D team built on top of the HELIOS Base framework.

## Development team and timeline
* [Ryan McLoughlin](https://www.linkedin.com/in/ryan-mcloughlin-b4947b295/), Final Year CS student in QUB, UK

* [Prof. Dr. Vahid Garousi](https://www.vgarousi.com), Professor of Software Engineering, Queen’s University Belfast, UK

## Technical Documentation

See [this PDF file](https://github.com/vgoruslu/robocup-football-simulation-QUB-2025-26/blob/main/Final%20Report-Ryan%20McLoughlin.pdf)

## Repository Structure

- `src/` - Custom Ryan FC team source code and configuration files
- `scripts/` - Scripts used to run matches and experiments
- `analysis/` - Python scripts used to analyse match results
- `docs/` - Project dashboard and html files
- `index.html` - Project dashboard hosted with GitHub Pages

## External Dependencies

The following dependencies are required but are not included in this repository:

- HELIOS Base
- librcsc
- rcssserver
- soccerwindow2
- Boost 1.38 or later
- g++ / build-essential

## Development Environment

This project was developed using WSL Ubuntu with a local RoboCup Soccer 2D simulation environment.

## Notes

Third-party dependencies such as `librcsc`, `helios-base`, `rcssserver`, and `soccerwindow2` are not included in this repository.

Only the modified team code, project-specific scripts, analysis tools, documentation, and dashboard files are included.

Generated match logs and output files are excluded from version control.
