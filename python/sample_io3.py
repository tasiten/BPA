#実行時にplyファイルを引数として設定できる．
#また点数，メッシュ数を最後に表示する
import sys
import struct

with open(sys.argv[1], 'rb') as f:
    # read header
    while True:
        line = f.readline()
        print (line)
        if b'end_header' in line:
            break
        if b'vertex ' in line:
            vnum = int(line.split(b' ')[-1]) # num of vertices
        if b'face ' in line:
            fnum = int(line.split(b' ')[-1]) # num of faces

    try:
        # read vertices
        for i in range(vnum):
            for j in range(3):
                print (struct.unpack('f', f.read(4))[0], end=' ')
            print ("")
    except Exception as e:
        print("頂点表示でエラーが発生しました．")
        print(e)

    try:
        # read faces
        for i in range(fnum):
            n = struct.unpack('B', f.read(1))[0]
            for j in range(n):
                print (struct.unpack('i', f.read(4))[0], end=' ')
            print ("")
    except Exception as e:
        print("面表示でエラーが発生しました．")
        print(e)
    try:
        #print number of vertices and faces
        print("Number of Vertices:" + str(vnum))
        print("Numver of Faces:" + str(fnum))
    except Exception as e:
        print(e)

