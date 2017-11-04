import os
import subprocess

def ProcessTestFile(strFile):
    f = open(strFile)


for file in os.listdir("../spec_tests"):
    if (not file.endswith(".wast")):
        continue
    ProcessTestFile("../spec_tests" + file)
