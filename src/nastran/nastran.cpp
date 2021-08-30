#include <vsgXchange/nastran.h>
#include <vsg/all.h>
#include <iostream>
#include <regex>
#include <fstream>
#include <map>


namespace NastranImplementation
{

const char* vertSource = R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) in vec3 vsg_Vertex;
layout(location = 1) in float vsg_Temperature;

layout(location = 0) out float temp;

out gl_PerVertex{
    vec4 gl_Position;
};

void main() {
    gl_Position = (pc.projection * pc.modelview) * vec4(vsg_Vertex, 1.0f);
    temp = vsg_Temperature;
}
)";

const char* fragSource = R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in float temp;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(temp, temp, temp, 1.0f);
}
)";

bool debugOutput = false;


void normalizeTemperatures(std::vector<float>& temperatures) 
{
    float maxTemp = *std::max_element(temperatures.begin(), temperatures.end());
    float minTemp = *std::min_element(temperatures.begin(), temperatures.end());

    if (debugOutput) {
        std::cout << "maxTemp: " << maxTemp << std::endl;
        std::cout << "minTemp: " << minTemp << std::endl;
    }
    
    //Normalizing Temps between 0 and 1
    for (auto& temp : temperatures)
    {
        float normalizedTemp = (temp - minTemp) / (maxTemp - minTemp);
        if(debugOutput) std::cout << "normalize temp: " << temp << " -> " << normalizedTemp << std::endl;
        temp = normalizedTemp;
    }

}

void parseGridsToIDAndVec3Vec(const std::string& line, std::vector<vsg::vec3>& gridList, std::vector<int>& idList) 
{
    std::regex regex(",");

    std::vector<std::string> out(
        std::sregex_token_iterator(line.begin(), line.end(), regex, -1),
        std::sregex_token_iterator()
    );

    int id = 0;
    vsg::vec3 vec3Value(0.f, 0.f, 0.f);

    int index = 0;
    for (auto& s : out) {
        switch (index)
        {
        case 1: 
            id = std::stoi(s);
            idList.push_back(id);
            break;

        case 3: 
            vec3Value.x = std::stof(s); 
            break;
        case 4: 
            vec3Value.y = std::stof(s); 
            break;
        case 5: 
            vec3Value.z = std::stof(s); 
            break;
        }
        index++;
    }
    gridList.push_back(vec3Value);

    if (debugOutput) std::cout << "Grid: [" << id << "] " << vec3Value[0] << " " << vec3Value[1] << " " << vec3Value[2] << std::endl;
}

void parseTRIAToIDList(const std::string& line, std::vector<int>& idArray) 
{
    std::regex regex(",");

    std::vector<std::string> out(
        std::sregex_token_iterator(line.begin(), line.end(), regex, -1),
        std::sregex_token_iterator()
    );

    if (debugOutput) std::cout << "TRIA: ";

    int index = 0;
    for (auto& s : out) {
        if (index <= 5 && index > 2) {
            int id = std::stoi(s);
            idArray.push_back(id);
            if (debugOutput) std::cout << id << " ";
        }
        index++;
    }
    if (debugOutput) std::cout << std::endl;
}

void parseQUADToIDList(const std::string& line, std::vector<int>& idArray) 
{
    std::regex regex(",");

    std::vector<int> unfoldIndices{0,1,2,2,3,0};
    std::vector<int> quadIndices;

    std::vector<std::string> out(
        std::sregex_token_iterator(line.begin(), line.end(), regex, -1),
        std::sregex_token_iterator()
    );

    if (debugOutput) std::cout << "QUAD: ";

    int index = 0;
    for (auto& s : out) {
        if (index <= 6 && index > 2) {
            int id = std::stoi(s);
            quadIndices.push_back(id); 
            if (debugOutput) std::cout << id << " ";
        }
        index++;
    }

    if (debugOutput) std::cout << " -> ";

    for (auto unfoldIndex : unfoldIndices)
    {
        int unfoldedID = quadIndices[unfoldIndex];
        idArray.push_back(unfoldedID);
        if (debugOutput) std::cout << unfoldedID << " ";
    }
    if (debugOutput) std::cout << std::endl;
}

void parseTempToIDandFloatVec(const std::string& line, std::vector<float>& temperatureList, std::vector<int>& idList) 
{
    std::regex regex(",");

    std::vector<std::string> out(
        std::sregex_token_iterator(line.begin(), line.end(), regex, -1),
        std::sregex_token_iterator()
    );

    int id = 0;
    float temperature = 0.f;

    int index = 0;
    for (auto& s : out) {
        switch (index)
        {

        case 2: 
            id = std::stoi(s);
            idList.push_back(id);
            break;
        
        case 3: 
            temperature = std::stof(s);
            temperatureList.push_back(temperature);
            break;
        

        }
        index++;
    }

    if (debugOutput) std::cout << "Temp: [" << id << "] " << temperature << std::endl;
}

void printVec3(int index, const vsg::vec3 input)
{
    if (debugOutput) std::cout << "[" << index << "] " << input.x << " / " << input.y << " / " << input.z << std::endl;
}

vsg::ref_ptr<vsg::Object> read(std::istream& stream)
{
    std::string line;

    //values GRID,VertexIndex,,X,Y,Z,,,, -> vec3(X,Y,Z)
    std::vector<vsg::vec3> gridList;
    //keys GRID,VertexIndex,,X,Y,Z,,,, -> VertexIndex
    std::vector<int> gridIDs;

    //values TEMP,TempNum,TempIndex,TempValue,,,,,, -> (TempValue)
    std::vector<float> temperatureList;
    //keys TEMP,TempNum,TempIndex,TempValue,,,,,, -> (TempIndex)
    std::vector<int> temperatureIDs;

    //Simple list of all Trias and Quads.
    //"Unfolded" because Quads get parsed into 2 triangles
    //Still contains the IDs, has to be converted to an IndexBuffer
    std::vector<int> unfoldedIDList;

    while (getline(stream, line))
    {
        if (line.find("GRID,") != std::string::npos)
        {
            parseGridsToIDAndVec3Vec(line, gridList, gridIDs);
        }
        else if (line.find("CTRIA3,") != std::string::npos)
        {
            parseTRIAToIDList(line, unfoldedIDList);
        }
        else if (line.find("QUAD4,") != std::string::npos)
        {
            parseQUADToIDList(line, unfoldedIDList);
        }
        else if (line.find("TEMP,") != std::string::npos)
        {
            parseTempToIDandFloatVec(line, temperatureList, temperatureIDs);
        }
    }


    //Do TempIndices and VertexIndices match?
    std::sort(gridIDs.begin(), gridIDs.end());
    std::sort(temperatureIDs.begin(), temperatureIDs.end());

    if (temperatureIDs != gridIDs) {
        if (debugOutput) std::cout << "Broken Nastran file detected: Vertex Indices and Temperature Indices do not have a Bijective mapping. Skipping load." << std::endl;
        return { };
    }

    //Normalize Temperatures
    normalizeTemperatures(temperatureList);

    //Normalized map of ordered ids, e.g. nastran grids are 5, 10, 23 -> map to -> 0, 1, 2, to fit into an index buffer
    int normalizeIDIndex = 0;
    std::map<int, int> gridToIndexMap;
    for (auto gridID : gridIDs) {
        std::pair<int, int> normalizedPair;
        normalizedPair.first = gridID;
        normalizedPair.second = normalizeIDIndex;
        gridToIndexMap.insert(normalizedPair);
        if (debugOutput) std::cout << "GridMap: " << gridID << " -> " << normalizeIDIndex << std::endl;
        normalizeIDIndex++;
    }

    auto vsg_vertices = vsg::vec3Array::create(static_cast<uint32_t>(gridList.size()));
    auto vsg_temperatures = vsg::floatArray::create(static_cast<uint32_t>(gridList.size()));
    for (size_t i = 0; i < gridList.size(); ++i)
    {
        (*vsg_vertices)[i] = gridList[i];
        (*vsg_temperatures)[i] = temperatureList[i];
    }

    //convert idList to indexBuffer
    for (size_t i = 0; i < unfoldedIDList.size(); ++i) {
        unfoldedIDList[i] = gridToIndexMap[unfoldedIDList[i]];
        if (debugOutput) std::cout << "Map Grid ID [" << i << "] -> to IndexBufferID -> " << unfoldedIDList[i] << std::endl;
    }

    if (debugOutput) std::cout << "unfolded indexBuffer size: " << unfoldedIDList.size() << std::endl;

    vsg::ref_ptr<vsg::intArray> vsg_indices = vsg::intArray::create(unfoldedIDList.size());
    for(size_t i = 0; i<unfoldedIDList.size(); ++i)
    {
        vsg_indices->set(i, unfoldedIDList[i]);
    }


    vsg::ref_ptr<vsg::ShaderStage> vertexShader = vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertSource);
    vsg::ref_ptr<vsg::ShaderStage> fragmentShader = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", fragSource);
    if (!vertexShader || !fragmentShader)
    {
        if (debugOutput) std::cout << "Could not create shaders." << std::endl;
        return { };
    }

    vsg::PushConstantRanges pushConstantRanges{ //
        {VK_SHADER_STAGE_VERTEX_BIT, 0, 128} // projection view, and model matrices, actual push constant calls automatically provided by the VSG's DispatchTraversal
    };

    vsg::VertexInputState::Bindings vertexBindingsDescriptions{
        VkVertexInputBindingDescription{0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX}, // nastran vertex
        VkVertexInputBindingDescription{1, sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX}, // nastran vertex
    };

    vsg::VertexInputState::Attributes vertexAttributeDescriptions{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, // vertex data
        VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R32_SFLOAT, 0}, // temprature data
    };

    vsg::GraphicsPipelineStates pipelineStates{
        vsg::VertexInputState::create(vertexBindingsDescriptions, vertexAttributeDescriptions),
        vsg::InputAssemblyState::create(),
        vsg::RasterizationState::create(),
        vsg::MultisampleState::create(),
        vsg::ColorBlendState::create(),
        vsg::DepthStencilState::create() };

    auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{ }, pushConstantRanges);
    auto graphicsPipeline = vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{ vertexShader, fragmentShader }, pipelineStates);
    auto bindGraphicsPipeline = vsg::BindGraphicsPipeline::create(graphicsPipeline);


    // create StateGroup as the root of the scene/command graph to hold the GraphicsProgram, and binding of Descriptors to decorate the whole graph
    auto stategroup = vsg::StateGroup::create();
    stategroup->add(bindGraphicsPipeline);

    // setup geometry
    auto vid = vsg::VertexIndexDraw::create();
    vid->assignArrays({ vsg_vertices, vsg_temperatures });
    vid->assignIndices(vsg_indices);
    vid->indexCount = vsg_indices->size();
    vid->instanceCount = 1;

    // add drawCommands to transform
    stategroup->addChild(vid);

    if (debugOutput) std::cout << "Nastran file loaded" << std::endl;

    return stategroup;
}

} // NastranImplementation namespace

vsgXchange::nastran::nastran()
{
}

vsg::ref_ptr<vsg::Object> vsgXchange::nastran::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
{
    auto ext = vsg::lowerCaseFileExtension(filename);
    if (ext != "nas") return {};

    vsg::Path filenameToUse = vsg::findFile(filename, options);
    if (filenameToUse.empty()) return {};

    std::ifstream stream(filenameToUse.c_str(), std::ios::in);
    if (!stream) return { };

    return NastranImplementation::read(stream);
}

vsg::ref_ptr<vsg::Object> vsgXchange::nastran::read(std::istream& stream, vsg::ref_ptr<const vsg::Options> options) const
{
    if (options->extensionHint != "nas") return {};

    return NastranImplementation::read(stream);
}


bool vsgXchange::nastran::getFeatures(Features& features) const
{
    features.extensionFeatureMap["nas"] = static_cast<vsg::ReaderWriter::FeatureMask>(vsg::ReaderWriter::READ_FILENAME | vsg::ReaderWriter::READ_ISTREAM);
    return true;
}
