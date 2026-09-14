.PHONY: ci_bd_% hw_bd_% project

CI_COMPOSE = docker-compose.ci.yml
HW_COMPOSE = docker-compose.hardware.yml

ci_bd_%:
	PROJECT=esp-idf-projects/$*/ docker compose -f $(CI_COMPOSE) up --abort-on-container-exit --exit-code-from idf-builder

hw_bd_%:
	PROJECT=esp-idf-projects/$*/ docker compose -f $(HW_COMPOSE) up --abort-on-container-exit --exit-code-from idf-builder


project: 
	@echo "Build Projects through CI:" 
	@echo "make ci_bd_hello_world" 
	@echo "make ci_bd_rtos_hq" 
	@echo "make ci_bd_rtos_task_test" 
	@echo "make ci_bd_sample-project" 
	@echo "" 
	@echo "Build Projects through Hardware :" 
	@echo "make hw_bd_hello_world" 
	@echo "make hw_bd_rtos_hq" 
	@echo "make hw_bd_rtos_task_test" 
	@echo "make hw_bd_sample-project"