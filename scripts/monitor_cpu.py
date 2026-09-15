import psutil

print("cpu,mem")

while True:
    cpu = psutil.cpu_percent(interval=1, percpu=False)
    mem = psutil.virtual_memory().percent
    print("{},{}".format(cpu, mem))
