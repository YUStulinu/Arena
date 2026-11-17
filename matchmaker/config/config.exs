import Config
# the tests start the processes they need themselves
config :arena_mm, start_services: config_env() != :test
