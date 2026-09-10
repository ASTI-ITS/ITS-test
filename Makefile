.PHONY: bd_rtos_hq bd_rtos_task_test bd_hello_world bd_sample-project

# Added PROJECT=esp-idf-projects/ to specify what what project to build. This is needed because the docker-compose.ci.yml file is in the root of the repo, and the projects are in a subdirectory.

bd_rtos_hq:
	PROJECT=esp-idf-projects/rtos_hq/ docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder

bd_rtos_task_test:
	PROJECT=esp-idf-projects/rtos_task_test/ docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder

bd_hello_world:
	PROJECT=esp-idf-projects/hello_world/ docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder

bd_sample-project:
	PROJECT=esp-idf-projects/sample-project/ docker compose -f docker-compose.ci.yml up --abort-on-container-exit --exit-code-from idf-builder