/*
www.sourceforge.net/projects/dfhack
Copyright (c) 2009 Petr Mrázek (peterix), Kenneth Ferland (Impaler[WrG]), dorf

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include "DFCommonInternal.h"
#include <errno.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <time.h>
#include "shmserver/dfconnect.h"
using namespace DFHack;

class SHMProcess::Private
{
    public:
    Private()
    {
        my_datamodel = NULL;
        my_descriptor = NULL;
        my_pid = 0;
        my_window = NULL;
        attached = false;
        suspended = false;
        identified = false;
    };
    ~Private(){};
    DataModel* my_datamodel;
    memory_info * my_descriptor;
    DFWindow * my_window;
    uint32_t my_pid;
    char *my_shm;
    int my_shmid;
    
    bool attached;
    bool suspended;
    bool identified;
    bool validate(char * exe_file, uint32_t pid, vector <memory_info> & known_versions);
    bool waitWhile (DF_PINGPONG state);
};

bool SHMProcess::Private::waitWhile (DF_PINGPONG state)
{
    uint32_t cnt = 0;
    struct shmid_ds descriptor;
    while (((shm_cmd *)my_shm)->pingpong == state)
    {
        if(cnt == 10000)
        {
            shmctl(my_shmid, IPC_STAT, &descriptor); 
            if(descriptor.shm_nattch == 1)// DF crashed?
            {
                ((shm_cmd *)my_shm)->pingpong = DFPP_RUNNING;
                attached = suspended = false;
                return false;
            }
            else
            {
                cnt = 0;
            }
        }
        cnt++;
    }
    if(((shm_cmd *)my_shm)->pingpong == DFPP_SV_ERROR)
    {
        ((shm_cmd *)my_shm)->pingpong = DFPP_RUNNING;
        attached = suspended = false;
        cerr << "shm server error!" << endl;
        assert (false);
        return false;
    }
    return true;
}

SHMProcess::SHMProcess(uint32_t pid, int shmid, char * shm, vector <memory_info> & known_versions)
: d(new Private())
{
    char exe_link_name [256];
    char target_name[1024];
    int target_result;
    
    sprintf(exe_link_name,"/proc/%d/exe", pid);
    
    // resolve /proc/PID/exe link
    target_result = readlink(exe_link_name, target_name, sizeof(target_name)-1);
    if (target_result == -1)
    {
        perror("readlink");
        return;
    }
    // make sure we have a null terminated string...
    target_name[target_result] = 0;
    
    // is this the regular linux DF?
    // create linux process, add it to the vector
    d->validate(target_name,pid,known_versions );
    d->my_shmid = shmid;
    d->my_shm = shm;
    d->my_window = new DFWindow(this);
    // make sure we restart the process
    ((shm_cmd *)d->my_shm)->pingpong = DFPP_RUNNING;
}

bool SHMProcess::isSuspended()
{
    return d->suspended;
}
bool SHMProcess::isAttached()
{
    return d->attached;
}

bool SHMProcess::isIdentified()
{
    return d->identified;
}

bool SHMProcess::Private::validate(char * exe_file,uint32_t pid, vector <memory_info> & known_versions)
{
    md5wrapper md5;
    // get hash of the running DF process
    string hash = md5.getHashFromFile(exe_file);
    vector<memory_info>::iterator it;
    cerr << exe_file << " " << hash <<  endl;
    // iterate over the list of memory locations
    for ( it=known_versions.begin() ; it < known_versions.end(); it++ )
    {
        if(hash == (*it).getString("md5")) // are the md5 hashes the same?
        {
            memory_info * m = &*it;
            my_datamodel =new DMLinux40d();
            my_descriptor = m;
            my_pid = pid;
            identified = true;
            cerr << "identified " << m->getVersion() << endl;
            return true;
        }
    }
    return false;
}

SHMProcess::~SHMProcess()
{
    if(d->attached)
    {
        detach();
    }
    // destroy data model. this is assigned by processmanager
    if(d->my_datamodel)
        delete d->my_datamodel;
    if(d->my_window)
        delete d->my_window;
    delete d;
}


DataModel *SHMProcess::getDataModel()
{
    return d->my_datamodel;
}

memory_info * SHMProcess::getDescriptor()
{
    return d->my_descriptor;
}

DFWindow * SHMProcess::getWindow()
{
    return d->my_window;
}

int SHMProcess::getPID()
{
    return d->my_pid;
}

//FIXME: implement
bool SHMProcess::getThreadIDs(vector<uint32_t> & threads )
{
    return false;
}

//FIXME: cross-reference with ELF segment entries?
void SHMProcess::getMemRanges( vector<t_memrange> & ranges )
{
    char buffer[1024];
    char permissions[5]; // r/-, w/-, x/-, p/s, 0
    
    sprintf(buffer, "/proc/%lu/maps", d->my_pid);
    FILE *mapFile = ::fopen(buffer, "r");
    uint64_t offset, device1, device2, node;
    
    while (fgets(buffer, 1024, mapFile))
    {
        t_memrange temp;
        temp.name[0] = 0;
        sscanf(buffer, "%llx-%llx %s %llx %2llu:%2llu %llu %s",
               &temp.start,
               &temp.end,
               (char*)&permissions,
               &offset, &device1, &device2, &node,
               (char*)&temp.name);
        temp.read = permissions[0] == 'r';
        temp.write = permissions[1] == 'w';
        temp.execute = permissions[2] == 'x';
        ranges.push_back(temp);
    }
}

bool SHMProcess::suspend()
{
    int status;
    if(!d->attached)
        return false;
    if(d->suspended)
        return true;
    ((shm_cmd *)d->my_shm)->pingpong = DFPP_SUSPEND;
    if(!d->waitWhile(DFPP_SUSPEND))
        return false;
    d->suspended = true;
    return true;
}

bool SHMProcess::forceresume()
{
    return resume();
}

bool SHMProcess::resume()
{
    if(!d->attached)
        return false;
    if(!d->suspended)
        return true;
    ((shm_cmd *)d->my_shm)->pingpong = DFPP_RUNNING;
    d->suspended = false;
    return true;
}


bool SHMProcess::attach()
{
    int status;
    if(g_pProcess != NULL)
    {
        cerr << "there's already a different process attached" << endl;
        return false;
    }
    d->attached = true;
    if(suspend())
    {
        d->suspended = true;
        g_pProcess = this;
        return true;
    }
    d->attached = false;
    cerr << "unable to suspend" << endl;
    return false;
}

bool SHMProcess::detach()
{
    if(!d->attached) return false;
    if(d->suspended) resume();
    d->attached = false;
    d->suspended = false;
}


// FIXME: use recursion
void SHMProcess::read (const uint32_t offset, const uint32_t size, uint8_t *target)
{
    assert (size < (SHM_SIZE - sizeof(shm_read)));
    ((shm_read *)d->my_shm)->address = offset;
    ((shm_read *)d->my_shm)->length = size;
    ((shm_read *)d->my_shm)->pingpong = DFPP_READ;
    d->waitWhile(DFPP_READ);
    memcpy (target, d->my_shm + sizeof(shm_ret_data),size);
}

uint8_t SHMProcess::readByte (const uint32_t offset)
{
    ((shm_read_small *)d->my_shm)->address = offset;
    ((shm_read_small *)d->my_shm)->pingpong = DFPP_READ_BYTE;
    d->waitWhile(DFPP_READ_BYTE);
    return ((shm_retval *)d->my_shm)->value;
}

void SHMProcess::readByte (const uint32_t offset, uint8_t &val )
{
    ((shm_read_small *)d->my_shm)->address = offset;
    ((shm_read_small *)d->my_shm)->pingpong = DFPP_READ_BYTE;
    d->waitWhile(DFPP_READ_BYTE);
    val = ((shm_retval *)d->my_shm)->value;
}

uint16_t SHMProcess::readWord (const uint32_t offset)
{
    ((shm_read_small *)d->my_shm)->address = offset;
    ((shm_read_small *)d->my_shm)->pingpong = DFPP_READ_WORD;
    d->waitWhile(DFPP_READ_WORD);
    return ((shm_retval *)d->my_shm)->value;
}

void SHMProcess::readWord (const uint32_t offset, uint16_t &val)
{
    ((shm_read_small *)d->my_shm)->address = offset;
    ((shm_read_small *)d->my_shm)->pingpong = DFPP_READ_WORD;
    d->waitWhile(DFPP_READ_WORD);
    val = ((shm_retval *)d->my_shm)->value;
}

uint32_t SHMProcess::readDWord (const uint32_t offset)
{
    ((shm_read_small *)d->my_shm)->address = offset;
    ((shm_read_small *)d->my_shm)->pingpong = DFPP_READ_DWORD;
    d->waitWhile(DFPP_READ_DWORD);
    return ((shm_retval *)d->my_shm)->value;
}
void SHMProcess::readDWord (const uint32_t offset, uint32_t &val)
{
    ((shm_read_small *)d->my_shm)->address = offset;
    ((shm_read_small *)d->my_shm)->pingpong = DFPP_READ_DWORD;
    d->waitWhile(DFPP_READ_DWORD);
    val = ((shm_retval *)d->my_shm)->value;
}

/*
 * WRITING
 */

void SHMProcess::writeDWord (uint32_t offset, uint32_t data)
{
    ((shm_write_small *)d->my_shm)->address = offset;
    ((shm_write_small *)d->my_shm)->value = data;
    ((shm_write_small *)d->my_shm)->pingpong = DFPP_WRITE_DWORD;
    d->waitWhile(DFPP_WRITE_DWORD);
}

// using these is expensive.
void SHMProcess::writeWord (uint32_t offset, uint16_t data)
{
    ((shm_write_small *)d->my_shm)->address = offset;
    ((shm_write_small *)d->my_shm)->value = data;
    ((shm_write_small *)d->my_shm)->pingpong = DFPP_WRITE_WORD;
    d->waitWhile(DFPP_WRITE_WORD);
}

void SHMProcess::writeByte (uint32_t offset, uint8_t data)
{
    ((shm_write_small *)d->my_shm)->address = offset;
    ((shm_write_small *)d->my_shm)->value = data;
    ((shm_write_small *)d->my_shm)->pingpong = DFPP_WRITE_BYTE;
    d->waitWhile(DFPP_WRITE_BYTE);
}

void SHMProcess::write (uint32_t offset, uint32_t size, const uint8_t *source)
{
    ((shm_write *)d->my_shm)->address = offset;
    ((shm_write *)d->my_shm)->length = size;
    memcpy(d->my_shm+sizeof(shm_write),source, size);
    ((shm_write *)d->my_shm)->pingpong = DFPP_WRITE;
    d->waitWhile(DFPP_WRITE);
}

// FIXME: butt-fugly
const std::string SHMProcess::readCString (uint32_t offset)
{
    std::string temp;
    char temp_c[256];
    int counter = 0;
    char r;
    do
    {
        r = readByte(offset+counter);
        temp_c[counter] = r;
        counter++;
    } while (r && counter < 255);
    temp_c[counter] = 0;
    temp = temp_c;
    return temp;
}

