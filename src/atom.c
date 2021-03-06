#include "atom.h"
#include "timer.h"
#include "parameter.h"
#include "cell.h"
#include "system.h"
#include "lattice.h"
#include "energy.h"
#include "datacomm.h"
#include "random.h"
#include "info.h"
#include "mympi.h"

#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <mpi.h>

static void dataToSmBuf(struct SystemStr* sys);
static void processSmData(struct SystemStr* sys, void *smbuf, enum Neighbor dimen);

// 初始化原子信息结构体
void initAtoms(struct CellStr* cells, Atom** ato){

	*ato = (Atom*)malloc(sizeof(Atom));
    Atom* atoms = *ato;

   	int maxAtomNum = MAXPERCELL*cells->totalCellNum;
	
	atoms->myNum = 0;
   	atoms->totalNum = 0;

   	atoms->pos = (double3*) malloc(maxAtomNum*sizeof(double3));
   	atoms->momenta = (double3*) malloc(maxAtomNum*sizeof(double3));
   	atoms->force = (double3*) malloc(maxAtomNum*sizeof(double3));
   	atoms->pot = (double*)malloc(maxAtomNum*sizeof(double));
   	atoms->id = (int*)malloc(maxAtomNum*sizeof(int));

   	for (int i = 0; i < maxAtomNum; i++)
   	{
      	for(int j = 0; j< 3; j++){
      		atoms->pos[i][j] = 0.0;
      		atoms->momenta[i][j] = 0.0;
      		atoms->force[i][j] = 0.0;
      	}
      	atoms->pot[i] = 0.0;
      	atoms->id[i] = 0;
   	}
}

// 分配各原子到对应的细胞中
void distributeAtoms(struct SystemStr* sys, struct ParameterStr* para){
 
   	double latticeConst = sys->lattice->latticeConst;
   	int yLat = para->yLat;
   	int zLat = para->zLat;
   	double* myMin = sys->space->myMin;
   	double* myMax = sys->space->myMax;

   	double3 xyzpos;  // 原子坐标

   	double3 momenta; // 原子动量
   	momenta[0] = 0.0;
   	momenta[1] = 0.0;
   	momenta[2] = 0.0;
   
   	int n = 4;  // 每个晶胞4个原子
   	double3 displace[4] = { {0.25, 0.25, 0.25},
      	{0.25, 0.75, 0.75},
      	{0.75, 0.25, 0.75},
      	{0.75, 0.75, 0.25} };

   	// 分配原子
   	int begin[3];
   	int end[3];
   	for (int i = 0; i < 3; i++)
   	{
      	begin[i] = floor(myMin[i]/latticeConst);
      	end[i]   = ceil (myMax[i]/latticeConst);
   	}

   	for (int ix=begin[0]; ix<end[0]; ++ix)
      	for (int iy=begin[1]; iy<end[1]; ++iy)
         	for (int iz=begin[2]; iz<end[2]; ++iz)
            	for (int ib=0; ib<n; ++ib)
            	{
               		double xpos = (ix+displace[ib][0]) * latticeConst;
               		double ypos = (iy+displace[ib][1]) * latticeConst;
               		double zpos = (iz+displace[ib][2]) * latticeConst;
               		if (xpos < myMin[0] || xpos >= myMax[0]) continue;
               		if (ypos < myMin[1] || ypos >= myMax[1]) continue;
               		if (zpos < myMin[2] || zpos >= myMax[2]) continue;

               		// 计算原子的id
               		int id = ib+n*(iz+zLat*(iy+yLat*(ix)));

               		xyzpos[0] = xpos;
               		xyzpos[1] = ypos;
               		xyzpos[2] = zpos;

               		// 将此原子置于对应的细胞中,并初始化动量为0
               		assignAtom(id, xyzpos, sys, momenta);
            	}

   	// 利用mpi的reduce计算所有进程的总原子数量
   	//beginTimer(reduce);
   	MPI_Allreduce(&sys->atoms->myNum, &sys->atoms->totalNum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
   	//endTimer(reduce);
    //printTotalAtom(stdout,sys->atoms);

   	//assert(s->atoms->nGlobal == nb*nx*ny*nz);
}

// 将指定原子分配到对应的细胞中
void assignAtom(int id, double3 xyzpos, struct SystemStr* sys, double3 momenta){
    
    // 根据原子坐标找到对应的细胞
    int cell = findCellByCoord(sys->cells, sys->space, xyzpos);

    // if(getMyRank()==2){
    //     printf("cell :%d\n",cell );
    // }
    // 计算此原子为本空间第几个原子
    int n = cell*MAXPERCELL;
    n = n + sys->cells->atomNum[cell];
   
    // 若不在通信区域中，本空间总原子数加1
    if (cell < sys->cells->myCellNum)
        sys->atoms->myNum++;

    // 当前细胞中的原子数加1
    sys->cells->atomNum[cell]++;

    sys->atoms->id[n] = id;

    // 对原子的位置坐标、动量赋值
    for(int i =0; i<3 ;i++){
        sys->atoms->pos[n][i] = xyzpos[i];
        sys->atoms->momenta[n][i] = momenta[i];
    }
}

// 初始化体系的温度，即原子的速度
void initTemperature(struct SystemStr* sys, struct ParameterStr* para){

    // 指定温度
    double temper = para->initTemper;
    // 原子质量
    double atomM = sys->lattice->atomM; 
 
    // 本空间所有原子总动量
    double3 myMomenta = {0.0,0.0,0.0};

    // 整个体系所有原子总动量
    double3 globalMomenta = {0.0,0.0,0.0};

    // 给定原子一个随机的速度及动量，并计算本空间的所有原子总动量
    for (int nCell=0; nCell<sys->cells->myCellNum; nCell++)
        for (int n=MAXPERCELL*nCell, count=0; count<sys->cells->atomNum[nCell]; count++, n++)
        {
            double sigma = sqrt(kB * temper/atomM);
            uint64_t seed = mkSeed(sys->atoms->id[n], 123);
            sys->atoms->momenta[n][0] = atomM * sigma * gasdev(&seed);
            sys->atoms->momenta[n][1] = atomM * sigma * gasdev(&seed);
            sys->atoms->momenta[n][2] = atomM * sigma * gasdev(&seed);

            myMomenta[0] += sys->atoms->momenta[n][0];
            myMomenta[1] += sys->atoms->momenta[n][1];
            myMomenta[2] += sys->atoms->momenta[n][2];
        }

    // 保证体系的总动量为0，在计算力之前需要调整为0

    // AllReduce, 得到整个体系的总动量
    MPI_Allreduce(myMomenta, globalMomenta, 3, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    
    // 每个原子需要中和的动量值
    double3 adjustMomenta;
    for(int i=0; i<3; i++)
        adjustMomenta[i] = -1 * globalMomenta[i]/sys->atoms->totalNum;

    // 调整各原子动量
    for (int nCell=0; nCell<sys->cells->myCellNum; nCell++)
        for (int n=MAXPERCELL*nCell, count=0; count<sys->cells->atomNum[nCell]; count++, n++)
            for(int i=0 ;i<3 ;i++)
                sys->atoms->momenta[n][i] += adjustMomenta[i];

    // 调整总动量为0后，需要调整体系的温度为指定温度
    computeTotalKinetic(sys);
    // 调整前的系统温度
    double t = (2*sys->energy->kineticEnergy)/(sys->atoms->totalNum*kB*3); 
    // 校正因子
    double factor = sqrt(temper/t);

    // 调整温度,乘以校正因子
    for (int nCell=0; nCell<sys->cells->myCellNum; nCell++)
        for (int n=MAXPERCELL*nCell, count=0; count<sys->cells->atomNum[nCell]; count++, n++)
            for(int i=0 ;i<3 ;i++)
                sys->atoms->momenta[n][i] *= factor; 

    // 计算调整后的总动能
    computeTotalKinetic(sys);
    //printTemper(stdout, sys->energy, sys->atoms->totalNum);
}

// 调整原子所在细胞，并进行原子数据通信(去掉了序号排序)
void adjustAtoms(struct SystemStr* sys){

    // 清空本空间外的细胞
    for (int i=sys->cells->myCellNum; i<sys->cells->totalCellNum; i++)
        sys->cells->atomNum[i] = 0;

    // 调整原子所在细胞
    for (int nCell=0; nCell<sys->cells->myCellNum; nCell++)
        for (int n = nCell*MAXPERCELL,count=0; count< sys->cells->atomNum[nCell];)
        {
            int nCell2 = findCellByCoord(sys->cells, sys->space,sys->atoms->pos[n+count]);
            if (nCell2 == nCell){
                count++;
                continue;
            }   
            moveAtom(sys->cells, sys->atoms, count, nCell, nCell2);           
        }

    //int haloatoms=0;
    //for (int i=sys->cells->myCellNum; i<sys->cells->totalCellNum; i++)
    //    haloatoms+=sys->cells->atomNum[i];
    //printf("haloatoms:%d\n",haloatoms);
    MPI_Allreduce(&sys->atoms->myNum, &sys->atoms->totalNum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    //printTotalAtom(stdout,sys->atoms);
    //printf("adjust\n");

    dataToSmBuf(sys);

    // 与各邻居进程进行通信
    //enum Neighbor dimen;
    int neg_neighbor,pos_neighbor;
    int neg_dimen,pos_dimen;

    // 4个缓冲区，正负轴上发送缓冲区和接收缓冲区

    // 内存共享，直接取数据，而不是点对点通信
    //int bufsize = sys->datacomm->bufSize;
    char* PutBuf = (char *)sys->usrBuf;
    //char* negGetBuf = NULL;
    //char* posGetBuf = NULL;
   
    MPI_Aint r1,r2;
    int recv1,recv2,recv1_t;
    int t1,t2;
    char *getbuf1 = NULL;
    char *getbuf2= NULL;

    char *smbuf1 = NULL;
    char *smbuf2 = NULL;

    //开始计算通信时间
    beginTimer(communication);
    for(int dimen = 0;dimen<3;dimen++){

        neg_dimen = 2*dimen;
        pos_dimen = 2*dimen +1;
        //int n_dimen = dimen + ((dimen%2==0)?(1):(-1));
        neg_neighbor = sys->datacomm->neighborProc[neg_dimen];
        pos_neighbor = sys->datacomm->neighborProc[pos_dimen];

        // if(getMyRank()==2){
        //     // for(int i=0;i<6;i++)
        //     //     printf("%d \n",sys->datacomm->neighborProc[i] );
        //     printf("(dimen%2)?-1:1---%d \n", ((dimen%2==0)?(1):(-1)));
        //     printf("dimen:%d n_dimen:%d neighbor:%d\n",dimen,n_dimen,neighbor);
        // }

        int negPutSize=0,posPutSize=0;

        for (int i=0; i<sys->datacomm->commCellNum[neg_dimen]; i++)
            negPutSize += sys->cells->atomNum[sys->datacomm->commCells[neg_dimen][i]];
        for (int i=0; i<sys->datacomm->commCellNum[pos_dimen]; i++)
            posPutSize += sys->cells->atomNum[sys->datacomm->commCells[pos_dimen][i]];
        
        //printf("%d: \n",PutSize*sizeof(AtomData));
        //beginTimer(test);
        // MPI_Win_allocate_shared(PutSize*sizeof(AtomData), sizeof(char),
        //  MPI_INFO_NULL,MPI_COMM_WORLD, &PutBuf, &win);
        //endTimer(test);

        ///应该拿到外面去，不能每次都开辟一个新窗口，从头到尾都用同一个

        // 将数据加入发送缓冲区
        
        //beginTimer(adjustatom);
        memcpy(PutBuf,&negPutSize,sizeof(int));
        memcpy(PutBuf+sizeof(int),&posPutSize,sizeof(int));

        addSendData(sys, PutBuf+2*sizeof(int), neg_dimen);//应该先缓存然后一次性全部put???

        addSendData(sys, PutBuf+2*sizeof(int)+negPutSize*sizeof(AtomData), pos_dimen);
        //endTimer(adjustatom);
        //printf("%d: \n",num );
       MPI_Win_fence(0,sys->win2);
        //int pos_send = addSendData(sys, posSendBuf, dimen_POSI);
        //printf("addsend\n");
        // if (ifZeroRank())
        // {
        //     printf("%d %d %d %d\n",neg_send,pos_send,sizeof(AtomData),bufsize);
        // }

        // 调用mpi_sendrecv函数，与邻居进程发送与接收原子数据
        //printf("1\n");

    
       MPI_Win_shared_query(sys->win1,neg_neighbor, &r1, &t1, &smbuf1);
       processSmData(sys, smbuf1, pos_dimen);

       MPI_Win_shared_query(sys->win1,pos_neighbor, &r2, &t2, &smbuf2);
       processSmData(sys, smbuf2, neg_dimen);
 
       MPI_Win_fence(0,sys->win1); 

        MPI_Win_shared_query(sys->win2,neg_neighbor, &r1, &t1, &getbuf1);
        
        //printf("%d \n",recv );

        //printf("2\n");
        //if(dimen%2 == 0){
            memcpy((char *)&recv1_t,getbuf1,sizeof(int));
            memcpy((char *)&recv1,getbuf1+sizeof(int),sizeof(int));
            //printf("porc %d recv1: %d recv1_t: %d r1:%d\n",getMyRank(),recv1,recv1_t,r1);

            //beginTimer(test);
            //negGetBuf = (char *)malloc(recv1*sizeof(AtomData));
            //memcpy(negGetBuf,getbuf1+2*sizeof(int)+recv1_t*sizeof(AtomData),recv1*sizeof(AtomData));
            //printf("porc %d memcpy1 success\n",getMyRank());
            //endTimer(test);
            // MPI_Get(negGetBuf, recv1,
            //     MPI_BYTE, neighbor, 0,/*nextrank*(nextrank+1)/2,*/
            //     recv1, MPI_BYTE,win);
        //}
        //else{

           // printf("start recv2 query\n");
            MPI_Win_shared_query(sys->win2,pos_neighbor, &r2, &t2, &getbuf2);
            //printf("recv2 query success  r2:%d\n",r2);
            memcpy((char *)&recv2,getbuf2,sizeof(int));
            //printf("recv2: %d\n",recv2 );

            //beginTimer(test);
            //beginTimer(test);
            //posGetBuf = (char *)malloc(recv2*sizeof(AtomData));
            //memcpy(posGetBuf,getbuf2+2*sizeof(int),recv2*sizeof(AtomData));
            //endTimer(test);
            //printf("memcpy2 success\n");
            //endTimer(test);
            // MPI_Get(posGetBuf, recv2,
            //     MPI_BYTE, neighbor, 0,/*nextrank*(nextrank+1)/2,*/
            //     recv2, MPI_BYTE,win);
        //}
        // MPI_Status status1,status2;
        // MPI_Sendrecv(negSendBuf, neg_send*sizeof(AtomData), MPI_BYTE, neighbor_NEGA, 0,
        //         posRecvBuf, bufsize, MPI_BYTE, neighbor_POSI, 0,
        //         MPI_COMM_WORLD, &status1);
        // MPI_Get_count(&status1, MPI_BYTE, &pos_recv);
        // MPI_Sendrecv(posSendBuf, pos_send*sizeof(AtomData), MPI_BYTE, neighbor_POSI, 0,
        //         negRecvBuf, bufsize, MPI_BYTE, neighbor_NEGA, 0,
        //         MPI_COMM_WORLD, &status2);
        // MPI_Get_count(&status2, MPI_BYTE, &neg_recv);
       
        //printf("sendrecv\n");

        // if (ifZeroRank())
        // {
        //     printf("pos_recv:%d neg_recv:%d\n",pos_recv,neg_recv);
        // }
            //printf("3\n");
        // 处理接收到的原子数据，将原子分配至细胞中
        // if(dimen%2){
        //     printf("p %d:recv2:%d recv1:%d\n",getMyRank(),recv2,recv1 );
            
             procRecvData(sys, getbuf1+2*sizeof(int)+recv1_t*sizeof(AtomData), recv1);
             procRecvData(sys, getbuf2+2*sizeof(int), recv2);        
             //printf("p %d:procdata success\n",getMyRank());
        // }
             //free(posGetBuf);free(negGetBuf);
             //printf("4\n");
             MPI_Win_fence(0,sys->win2);     
    }
    endTimer(communication);

    // 通信结束，释放缓冲区
    //free(posGetBuf);free(negGetBuf);
}

// 将cell1中的第N个原子移动到cell2中
void moveAtom(struct CellStr* cells, Atom* atoms, int n, int cell1, int cell2){

    // 先将原子数据写入细胞cell2中
    int n1 = MAXPERCELL*cell1+n;
    int n2 = MAXPERCELL*cell2+cells->atomNum[cell2];
    for(int i=0;i<3;i++){
        atoms->pos[n2][i]=atoms->pos[n1][i];
        atoms->momenta[n2][i]=atoms->momenta[n1][i];
        atoms->force[n2][i]=atoms->force[n1][i];
    }
    atoms->pot[n2] = atoms->pot[n1];
    atoms->id[n2] = atoms->id[n1];

    // cell2中原子总数加1
    cells->atomNum[cell2]++;

    //assert

    // cell1中原子总数减1
    cells->atomNum[cell1]--;
    
    // 若cell1中还有原子，则将最后一个原子数据填补至被移动的原子处
    if (cells->atomNum[cell1]){
        n1 = MAXPERCELL*cell1+cells->atomNum[cell1];
        n2 = MAXPERCELL*cell1+n;
        for(int i=0;i<3;i++){
            atoms->pos[n2][i]=atoms->pos[n1][i];
            atoms->momenta[n2][i]=atoms->momenta[n1][i];
            atoms->force[n2][i]=atoms->force[n1][i];
        }
        atoms->pot[n2] = atoms->pot[n1];
        atoms->id[n2] = atoms->id[n1];
    }

    // 若原子移动出了本空间,则本空间总原子数减1
    if (cell2 > cells->myCellNum) //此处不应该是>=??????
        atoms->myNum--;
}

void dataToSmBuf(struct SystemStr* sys){

    int atomnum=0;

    //int allnum =0;

    AtomData* smbuf = (AtomData*)(sys->smBuf+6*sizeof(int)); 
    //int3 xyz;      

    for(int dimen=0;dimen<6;dimen++){

        int sharedCellNum = sys->datacomm->sharedCellNum[dimen];  
        int* sharedCells = sys->datacomm->sharedCells[dimen];

        for (int nCell=0; nCell<sharedCellNum; nCell++)
        {
            int cell = sharedCells[nCell];
            //getXYZByCell(sys->cells, xyz, cell);
            //int smCell = getSMCellByXYZ(sys->cells, xyz);
             //int m = smCell * MAXPERCELL;
            //atomnum += sys->cells->atomNum[cell];

            for (int n=cell*MAXPERCELL,count=0; count<sys->cells->atomNum[cell]; n++,count++)
            {
                for(int i=0;i<3;i++){
                    smbuf[atomnum].pos[i] = sys->atoms->pos[n][i];
                    smbuf[atomnum].momenta[i] = sys->atoms->momenta[n][i];
                }
                smbuf[atomnum].id  = sys->atoms->id[n];
                atomnum++;
            }
        }

        memcpy(sys->smBuf+dimen*sizeof(int),&atomnum,sizeof(int));

    }
}

void processSmData(struct SystemStr* sys, void *smbuf, enum Neighbor dimen){

    int atomnum_end = 0;
    int atomnum_start = 0;
    char *start =(char *)smbuf;
    memcpy((char *)&atomnum_end,start+dimen*sizeof(int),sizeof(int));


    if(dimen == 0)
        atomnum_start =0;
    else
        memcpy((char *)&atomnum_start,start+(dimen-1)*sizeof(int),sizeof(int));
    //printf("rank:%d atomnum_start:%d atomnum_end:%d\n ",getMyRank(),atomnum_start,atomnum_end);
    AtomData* buffer = (AtomData*) (smbuf+6*sizeof(int)); 

    double3 pos; //原子坐标
    double3 momenta; //原子动量
    int id;

    int* spacePos = sys->space->position;
    int* spaceNum = sys->space->globalProcNum;

    double3 boundaryAdjust;
    for(int i=0;i<3;i++)
        boundaryAdjust[i]= 0.0;

    if(spacePos[0] == 0 && dimen == X_POS)
        boundaryAdjust[0] = -sys->space->globalLength[0];
    if(spacePos[0] == spaceNum[0]-1 && dimen == X_NEG)
        boundaryAdjust[0] = 1.0*sys->space->globalLength[0];
    if(spacePos[1] == 0 && dimen == Y_POS)
        boundaryAdjust[1] = -sys->space->globalLength[1];
    if(spacePos[1] == spaceNum[1]-1 && dimen == Y_NEG)
        boundaryAdjust[1] = 1.0*sys->space->globalLength[1];
    if(spacePos[2] == 0 && dimen == Z_POS)
        boundaryAdjust[2] = -sys->space->globalLength[2];
    if(spacePos[2] == spaceNum[2]-1 && dimen == Z_NEG)
        boundaryAdjust[2] = 1.0*sys->space->globalLength[2];

    //printf("rank:%d test1\n ",getMyRank());
    for (int num=atomnum_start; num<atomnum_end; num++)
    {       
        for(int i=0;i<3;i++)
        {
            pos[i] = buffer[num].pos[i]+boundaryAdjust[i];
            momenta[i] = buffer[num].momenta[i];
        }
        id = buffer[num].id;

        //  if(getMyRank()==2){
        //     printf("num :%d \n",num );
        //     printf("pos: %g,%g,%g\n",pos[0],pos[1],pos[2] );
        //     printf("momenta: %g,%g,%g\n",momenta[0],momenta[1],momenta[2] );
        // }  
        assignAtom(id, pos, sys, momenta);
    }
     //printf("rank:%d test2\n ",getMyRank());
}

