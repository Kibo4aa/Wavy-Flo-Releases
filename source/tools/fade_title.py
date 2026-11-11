import json, sys
project = json.load(sys.stdin)
index = len(project['layers'])
print(json.dumps({'commands': [
 {'op': 'add_text', 'name': 'Python tool title', 'text': 'MADE WITH PYTHON', 'x': 640, 'y': 500},
 {'op': 'keyframe', 'layer': index, 'property': 'opacity', 'frame': 0, 'value': 0, 'easing': 'easeInOut'},
 {'op': 'keyframe', 'layer': index, 'property': 'opacity', 'frame': 45, 'value': 1}
]}))
