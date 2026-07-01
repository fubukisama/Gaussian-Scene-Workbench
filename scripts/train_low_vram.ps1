param(
    [string]$Scene = "..\datasets\my_scene",
    [string]$Model = "..\output\my_scene",
    [int]$Resolution = 4
)

$ErrorActionPreference = "Stop"

python train.py -s $Scene -m $Model -r $Resolution --data_device cpu

